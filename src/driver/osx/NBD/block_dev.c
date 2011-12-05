#include <libkern/libkern.h>
#include <device.h>
#include <common.h>
#include <nbd_ioctl.h>
#include <sys/kpi_socket.h>
#include <netinet/in.h>
#include <sys/conf.h>
#include <sys/disk.h>
#include <block_dev.h>


extern device devices[];


int  dev_open(dev_t bsd_dev, int flags, int devtype, proc_t proc)
{
	int ret;
	int minor_number;
	
	minor_number = minor(bsd_dev);
	device *dev = &(devices[minor_number]);
	printf("nbd: dev_open %d (%08x) minor=%d\n", bsd_dev, bsd_dev, minor_number);

	ret = -1;

	// get exclusive lock while we check device's state
	printf("nbd: dev_open: minor %d: proc: %p: spinlock...\n", minor_number, proc);
	lck_spin_lock(dev->lock);
	
	// locked by us

	if(dev->opened_by)
	{
		ret = EBUSY;
		goto unlock;
	}
	
	// successful
	
	dev->opened_by = proc;
	ret = 0;

unlock:
	lck_spin_unlock(dev->lock);
	printf("nbd: dev_open: minor %d: proc: %p: spinlock released\n", minor_number, proc);
	
out:
	printf("nbd: open: returning %d (%08x)\n", ret, ret);
	return ret;
}


int  dev_close(dev_t bsd_dev, int flags, int devtype, proc_t proc)
{
	int ret;
	int minor_number;
	
	minor_number = minor(bsd_dev);
	device *dev = &(devices[minor_number]);

	printf("nbd: dev_close %d (%08x) minor=%d dev=%p\n", bsd_dev, bsd_dev, minor(bsd_dev), dev);

	if(dev->opened_by != proc)
	{
		ret = EINVAL;
		goto out;
	}

	// can close; wipe out client-open state (keep the socket state)
	dev->opened_by = NULL;
	dev->client_block_size = BLOCK_SIZE;

out:
	return ret;
}


int  dev_size(dev_t bsd_dev)
{
	printf("nbd: dev_size minor=%d returning %d\n", minor(bsd_dev), BLOCK_SIZE);
	return BLOCK_SIZE;
}


void dev_strategy(buf_t bp)
{
	dev_t bsd_dev;
	long long byte_count;
	int minor_number;
	long long starting_block;
	long long starting_byte;
	int is_read;
	int is_write;
	device *dev;
	int ret;
	
	bsd_dev = buf_device(bp);
	minor_number = minor(bsd_dev);
	dev = &(devices[minor_number]);
	
	byte_count = buf_count(bp);
	starting_block = buf_blkno(bp);
	starting_byte = starting_block * BLOCK_SIZE;
	is_read = (buf_flags(bp) & B_READ) ? 1 : 0;
	is_write = ! is_read;  // there's no B_WRITE flag

	printf("nbd: strategy minor=%d read=%d write=%d start@ block=%lld offset=0x%016llx bytecount=%lld\n", minor_number, is_read, is_write, starting_block, starting_byte, byte_count);

	ret = EIO;
	goto out;

out:
	buf_seterror(bp, ret);
	buf_biodone(bp);
}


// XXX FIXME can't divide by block size; have to shift right this many bits.  How to get divdi3 linked into static binary?!
static int log_2(uint64_t x)
{
	int ret = 0;
	while(x > 0)
	{
		x = x >> 1;
		ret++;
	}
	
	return ret;
}


static void connect_call_complete(socket_t socket, void *cookie, int waitf)
{
	int minor_number;
	
	// XXX race
	minor_number = (int) (long) cookie;
	if(devices[minor_number].socket == socket)
	{
		printf("nbd: async socket: connected for device %d\n", minor_number);
		devices[minor_number].connect_completed = 1;
	}
	else
	{
		// oops!  referenced an old socket
	}
}


int  dev_ioctl_bdev(dev_t bsd_dev, u_long cmd, caddr_t data, int flags, proc_t proc)
{
	int ret;
	int result;
	int minor_number;
	ioctl_connect_device_t *ioctl_connect;
	struct sockaddr * server_sockaddr;
	socket_t socket;
	
	minor_number = minor(bsd_dev);
	
	device *dev = &(devices[minor_number]);
	printf("nbd: dev_ioctl_bdev %d (%08x) minor=%d dev=%p cmd=%08lx data=%p flags=%d proc=%p\n", bsd_dev, bsd_dev, minor(bsd_dev), dev, cmd, data, flags, proc);

	ret = 0;

	switch(cmd)
	{
	case DKIOCGETBLOCKSIZE:  // uint32_t: block size
		*(uint32_t *)data = dev->client_block_size;
		break;
	
	case DKIOCSETBLOCKSIZE:
		dev->client_block_size = *(uint32_t *) data;
		break;
	
	case DKIOCGETBLOCKCOUNT:  // uint64_t: block count
		if(! (dev->connect_completed && dev->socket) )
		{
			ret = ENXIO;
			break;
		}
		*(long long *)data = dev->size >> log_2((uint64_t)(dev->client_block_size));
		break;
	
	case IOCTL_CONNECT_DEVICE:
		ioctl_connect = (ioctl_connect_device_t *) data;
		server_sockaddr = (struct sockaddr *) &(ioctl_connect->server);

		// already connected?
		if(dev->socket)
		{
			ret = EBUSY;
			break;
		}

		// new socket
		result = sock_socket(PF_INET, SOCK_STREAM, IPPROTO_TCP, connect_call_complete, (void*) (long) minor_number, &(dev->socket));
		if(result)
		{
			printf("nbd: ioctl_connect: during sock_socket: %d\n", result);
			ret = result;
			break;
		}
		
		// try to connect (asynchronously)
		result = sock_connect(dev->socket, server_sockaddr, MSG_DONTWAIT);  // MSG_DONTWAIT -> don't block
		if(result != EINPROGRESS)
		{
			printf("nbd: ioctl_connect: during sock_connect: %d\n", result);
			sock_close(dev->socket);
			dev->socket = 0;
			ret = result;
			break;
		}
		
		break;

	case IOCTL_CONNECTIVITY_CHECK:  // uint32_t: connected boolean
		socket = dev->socket;
		if(! socket)
		{
			ret = ENXIO;
			break;
		}
		
		*(int *)data = sock_isconnected(socket);
		break;

	default:
		printf("nbd: ctl: ioctl: saying ENOTTY\n");
		ret = ENOTTY;
	}

	return ret;
}