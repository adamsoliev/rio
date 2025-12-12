#include <fcntl.h>
#include <iostream>
#include <liburing.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

using namespace std;

void fatal_error(const char *msg)
{
	fprintf(stderr, "%s\n", msg);
	exit(1);
}

void fatal_error(const string &msg)
{
	fprintf(stderr, "%s\n", msg.c_str());
	exit(1);
}

struct IOVecs
{
	vector<iovec> vecs;

	IOVecs(int qd, size_t buffer_size) : vecs(qd)
	{
		for (int i = 0; i < qd; i++)
		{
			void *buf;
			if (posix_memalign(&buf, buffer_size, buffer_size))
				fatal_error("Failed to allocate aligned buffer");
			vecs[i] = {buf, buffer_size};
		}
	}

	~IOVecs()
	{
		for (auto &vec : vecs)
			free(vec.iov_base);
	}
};

// defeault mode (interrupt-driven)
// based on https://github.com/axboe/liburing/blob/master/examples/io_uring-test.c

int default_mode()
{
	cout << "==============================================" << endl;
	cout << "================ Default mode ================" << endl;
	cout << "==============================================" << endl;

	int QD = 4;
	int i, fd, ret;
	struct stat sb;

	struct io_uring ring;
	int flag = 0;
	ret = io_uring_queue_init(QD, &ring, flag); //
	if (ret < 0)
		fatal_error(string("queue_init: ") + strerror(-ret));

	fd = open("/dev/nvme1n1", O_RDONLY | O_DIRECT);
	if (fd < 0)
		fatal_error("open failed");

	if (fstat(fd, &sb) < 0)
		fatal_error("fstat failed");

	IOVecs iovecs(QD, 4096);

	ssize_t fsize = 0;
	for (i = 0; i < QD; i++)
		fsize += 4096;

	off_t offset = 0;
	i = 0;
	do
	{
		struct io_uring_sqe *sqe;
		sqe = io_uring_get_sqe(&ring); //
		if (!sqe)                      // full
			break;
		io_uring_prep_readv(sqe, fd, &iovecs.vecs[i], 1, offset); //
		offset += iovecs.vecs[i].iov_len;
		i++;
		if (offset >= sb.st_size)
			break;
	} while (1);

	ret = io_uring_submit(&ring); //
	if (ret < 0)
		fatal_error(string("io_uring_submit: ") + strerror(-ret));
	else if (ret != i)
		fatal_error(string("io_uring_submit submitted less ") + to_string(ret));

	int pending = ret, done = 0;
	fsize = 0;
	for (i = 0; i < pending; i++)
	{
		struct io_uring_cqe *cqe;
		ret = io_uring_wait_cqe(&ring, &cqe); //
		if (ret < 0)
			fatal_error(string("io_uring_wait_cqe: ") + strerror(-ret));

		done++;
		ret = 0;
		if (cqe->res != 4096 && cqe->res + fsize != sb.st_size)
			fatal_error(string("ret=") + to_string(cqe->res) + ", wanted 4096");
		fsize += cqe->res;
		io_uring_cqe_seen(&ring, cqe); //
	}

	printf("Submitted=%d, completed=%d, bytes=%lu\n", pending, done, (unsigned long)fsize);
	close(fd);
	io_uring_queue_exit(&ring); //
	return 0;
}

// IORING_SETUP_IOPOLL (Busy-polling)
//
// IORING_SETUP_SQPOLL (Submission Queue Polling)
//
// IORING_SETUP_HYBRID_IOPOLL
//
// IORING_SETUP_COOP_TASKRUN: Reduces interrupts for single-threaded apps
//
// IORING_SETUP_SINGLE_ISSUER: Optimization hint for single-threaded submission
//
// IORING_SETUP_SQPOLL + IORING_SETUP_SQ_AFF: Pin SQPOLL thread to specific CPU

int main()
{
	cout << "\nBenchmark.." << endl;
	default_mode();

	return 0;
}
