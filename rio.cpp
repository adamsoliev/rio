#include <cstring>
#include <iostream>
#include <liburing.h>
#include <libnvme.h>
#include <getopt.h>
#include <string>
#include <fcntl.h>
#include <unistd.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <filesystem>
#include <regex>
#include <cstdlib>
#include <ctime>
#include <chrono>
#include <vector>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <iomanip>

static void fatal_error(const char *msg, int err = 0)
{
	std::cerr << "Fatal: " << msg;
	if (err != 0)
	{
		std::cerr << ": " << strerror(-err);
	}
	std::cerr << std::endl;
	std::exit(1);
}

struct Config
{
	const char *filename = nullptr;
	const char *type = nullptr;
	size_t size = 0;
	int iodepth = 0;
	size_t block_size = 0;
	bool passthrough = false; // O_DIRECT by default
};

struct NVMeDevice
{
	int fd = -1;
	uint32_t nsid = 1;     // namespace ID
	uint32_t lba_size = 0; // logical block size
	uint64_t nlba = 0;     // number of LBAs
};

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

struct IOContext
{
	void *buffer;
	TimePoint submit_time;
};

static size_t parse_size(const char *str)
{
	char *end;

	size_t val = strtoull(str, &end, 10);

	if (*end == '\0')
		return val;
	switch (*end)
	{
	case 'k':
	case 'K':
		return val * 1024;
	case 'm':
	case 'M':
		return val * 1024 * 1024;
	case 'g':
	case 'G':
		return val * 1024 * 1024 * 1024;
	default:
		std::cerr << "Invalid size suffix: " << end << std::endl;
		exit(1);
	}
}

static void usage(const char *prog)
{
	std::cerr << "Usage: " << prog << " [options]\n"
	          << "  --filename=<path>  Target device or file\n"
	          << "  --type=<type>      I/O pattern (randomread)\n"
	          << "  --size=<size>      Total workload size (e.g., 1g, 512m)\n"
	          << "  --iodepth=<num>    Queue depth\n"
	          << "  --bs=<size>        Block size (e.g., 4k)\n"
	          << "  --mode=<mode>      I/O mode: direct (default), passthrough\n";
	exit(1);
}

static Config parse_args(int argc, char **argv)
{
	Config cfg;

	static struct option long_options[] = {{"filename", required_argument, 0, 'f'},
	                                       {"type", required_argument, 0, 't'},
	                                       {"size", required_argument, 0, 's'},
	                                       {"iodepth", required_argument, 0, 'd'},
	                                       {"bs", required_argument, 0, 'b'},
	                                       {"mode", required_argument, 0, 'm'},
	                                       {0, 0, 0, 0}};

	int opt;
	while ((opt = getopt_long(argc, argv, "", long_options, nullptr)) != -1)
	{
		switch (opt)
		{
		case 'f':
			cfg.filename = optarg;
			break;
		case 't':
			cfg.type = optarg;
			break;
		case 's':
			cfg.size = parse_size(optarg);
			break;
		case 'd':
			cfg.iodepth = atoi(optarg);
			break;
		case 'b':
			cfg.block_size = parse_size(optarg);
			break;
		case 'm':
			if (strcmp(optarg, "direct") == 0)
			{
				cfg.passthrough = false;
			}
			else if (strcmp(optarg, "passthrough") == 0)
			{
				cfg.passthrough = true;
			}
			else
			{
				std::cerr << "Invalid mode: " << optarg << std::endl;
				usage(argv[0]);
			}
			break;
		default:
			usage(argv[0]);
		}
	}

	if (!cfg.filename || !cfg.type || cfg.size == 0 || cfg.iodepth == 0 || cfg.block_size == 0)
	{
		std::cerr << "Error: All parameters are required\n";
		usage(argv[0]);
	}

	if (strcmp(cfg.type, "randomread") != 0)
	{
		std::cerr << "Error: Only 'randomread' type is supported\n";
		exit(1);
	}

	return cfg;
}

static void setup_io_uring(struct io_uring *ring, int queue_depth)
{
	int ret = io_uring_queue_init(queue_depth, ring, 0);
	if (ret < 0)
	{
		fatal_error("io_uring_queue_init failed", ret);
	}
}

static std::string block_to_char_device(const char *path)
{
	namespace fs = std::filesystem;
	fs::path p = path;

	// Follow symlinks to get the actual device path
	std::error_code ec;
	while (fs::is_symlink(p, ec))
	{
		fs::path target = fs::read_symlink(p, ec);
		if (ec)
		{
			std::cerr << "Warning: Failed to resolve symlink " << p << ": " << ec.message() << std::endl;
			break;
		}
		// Handle relative symlinks
		if (target.is_relative())
		{
			p = p.parent_path() / target;
		}
		else
		{
			p = target;
		}
	}

	std::string resolved = p.string();

	// Validate it's an NVMe device
	bool has_nvme = resolved.find("nvme") != std::string::npos;
	bool has_ng = resolved.find("ng") != std::string::npos;

	if (!has_nvme && !has_ng)
	{
		std::cerr << "Warning: Device path '" << resolved << "' doesn't appear to be an NVMe device" << std::endl;
		return resolved;
	}

	// If already a character device (ng), return as-is
	if (has_ng && !has_nvme)
	{
		return resolved;
	}

	// Convert nvme -> ng for character device
	size_t pos = resolved.find("nvme");
	if (pos != std::string::npos)
	{
		resolved.replace(pos, 4, "ng");
	}

	return resolved;
}

static void open_nvme_ssd(const char *path, bool passthrough, NVMeDevice *nvme)
{
	std::string device_path = path;
	int flags = O_RDWR;
	if (passthrough)
	{
		device_path = block_to_char_device(path);
	}
	else
	{
		flags |= O_DIRECT;
	}
	nvme->fd = open(device_path.c_str(), flags);
	if (nvme->fd < 0)
	{
		fatal_error("Failed to open device");
	}

	if (passthrough)
	{
		// Query namespace ID
		if (ioctl(nvme->fd, NVME_IOCTL_ID, &nvme->nsid) < 0) // io control operation
		{
			close(nvme->fd);
			fatal_error("Failed to get namespace ID");
		}

		// Get namespace info using Identify Namespace command
		char identify_data[4096];
		memset(identify_data, 0, sizeof(identify_data));
		struct nvme_passthru_cmd cmd = {
		    .opcode = nvme_admin_identify,           // eg identify, read, write
		    .nsid = nvme->nsid,                      // namespace id
		    .addr = (uint64_t)identify_data,         // user-space address for data buffer
		    .data_len = 4096,                        // data buffer length
		    .cdw10 = NVME_IDENTIFY_CNS_NS,           // data structure being requested
		    .cdw11 = (uint32_t)NVME_CSI_NVM << 24,   // logical blocks store (vs key value store vs zones store)
		    .timeout_ms = NVME_DEFAULT_IOCTL_TIMEOUT // default timeout
		};

		if (ioctl(nvme->fd, NVME_IOCTL_ADMIN_CMD, &cmd) < 0)
		{
			close(nvme->fd);
			fatal_error("Failed to identify namespace");
		}
		struct nvme_id_ns *ns = (struct nvme_id_ns *)identify_data;
		int lba_format_index = ns->flbas & 0x0F;
		int lbads = ns->lbaf[lba_format_index].ds; // get 'data size' of LBA format entry

		nvme->lba_size = 1 << lbads; // eg ds=9, lba_size = 2^9 = 512 bytes
		nvme->nlba = ns->nsze;
	}
	else
	{
		// Direct mode: use standard block device ioctls
		uint64_t size_bytes;
		if (ioctl(nvme->fd, BLKGETSIZE64, &size_bytes) < 0)
		{
			close(nvme->fd);
			fatal_error("Failed to get device size");
		}

		int logical_block_size;
		if (ioctl(nvme->fd, BLKSSZGET, &logical_block_size) < 0)
		{
			close(nvme->fd);
			fatal_error("Failed to get logical block size");
		}

		nvme->lba_size = logical_block_size;
		nvme->nlba = size_bytes / nvme->lba_size;
		nvme->nsid = 0; // N/A
	}
}

static void *alloc_aligned_buffer(size_t size, size_t alignment)
{
	void *buf;
	if (posix_memalign(&buf, alignment, size) != 0)
	{
		fatal_error("Failed to allocate aligned buffer");
	}
	return buf;
}

static uint64_t random_lba(uint64_t max_lba, uint64_t block_lbas)
{
	if (max_lba <= block_lbas)
	{
		return 0;
	}
	uint64_t max_start = max_lba - block_lbas;
	return (uint64_t)rand() % (max_start + 1);
}

static void submit_read_direct(struct io_uring *ring, int fd, void *buf, size_t size, uint64_t offset, int buf_index)
{
	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
	if (!sqe)
	{
		fatal_error("Failed to get SQE");
	}
	io_uring_prep_read(sqe, fd, buf, size, offset);
	io_uring_sqe_set_data(sqe, (void *)(uintptr_t)buf_index);
}

static double percentile(std::vector<double> &sorted_latencies, double p)
{
	if (sorted_latencies.empty())
		return 0.0;
	double index = (p / 100.0) * (sorted_latencies.size() - 1);
	size_t lower = (size_t)index;
	size_t upper = lower + 1;
	if (upper >= sorted_latencies.size())
		return sorted_latencies.back();
	double frac = index - lower;
	return sorted_latencies[lower] * (1 - frac) + sorted_latencies[upper] * frac;
}

static void print_metrics(const std::vector<double> &latencies, double elapsed_sec, uint64_t completed_ops,
                          size_t block_size)
{
	double iops = completed_ops / elapsed_sec;
	double bandwidth_mbs = (completed_ops * block_size) / (elapsed_sec * 1024 * 1024);

	// Calculate latency statistics
	std::vector<double> sorted_lat = latencies;
	std::sort(sorted_lat.begin(), sorted_lat.end());

	double avg_lat = 0.0;
	if (!latencies.empty())
	{
		avg_lat = std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();
	}

	double p50 = percentile(sorted_lat, 50.0);
	double p95 = percentile(sorted_lat, 95.0);
	double p99 = percentile(sorted_lat, 99.0);
	double min_lat = sorted_lat.empty() ? 0.0 : sorted_lat.front();
	double max_lat = sorted_lat.empty() ? 0.0 : sorted_lat.back();

	std::cout << "\n";
	std::cout << "Results:\n";
	std::cout << "  IOPS:       " << std::fixed << std::setprecision(0) << iops << "\n";
	std::cout << "  Bandwidth:  " << std::fixed << std::setprecision(2) << bandwidth_mbs << " MB/s\n";
	std::cout << "  Latency (us):\n";
	std::cout << "    avg:      " << std::fixed << std::setprecision(2) << avg_lat << "\n";
	std::cout << "    min:      " << std::fixed << std::setprecision(2) << min_lat << "\n";
	std::cout << "    p50:      " << std::fixed << std::setprecision(2) << p50 << "\n";
	std::cout << "    p95:      " << std::fixed << std::setprecision(2) << p95 << "\n";
	std::cout << "    p99:      " << std::fixed << std::setprecision(2) << p99 << "\n";
	std::cout << "    max:      " << std::fixed << std::setprecision(2) << max_lat << "\n";
}

static void submit_read_passthrough(struct io_uring *ring, NVMeDevice *nvme, void *buf, uint64_t lba, uint32_t blocks,
                                    int buf_index)
{
	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
	if (!sqe)
	{
		fatal_error("Failed to get SQE");
	}

	// Prepare NVMe passthrough command
	struct nvme_passthru_cmd cmd = {};
	cmd.opcode = nvme_cmd_read;
	cmd.nsid = nvme->nsid;
	cmd.addr = (uint64_t)buf;
	cmd.data_len = blocks * nvme->lba_size;
	cmd.cdw10 = (uint32_t)lba;         // starting LBA lower 32 bits
	cmd.cdw11 = (uint32_t)(lba >> 32); // starting LBA upper 32 bits
	cmd.cdw12 = blocks - 1;            // number of blocks (0-based)

	// Use io_uring_prep_rw to setup IORING_OP_URING_CMD
	sqe->opcode = IORING_OP_URING_CMD;
	sqe->fd = nvme->fd;
	sqe->cmd_op = NVME_URING_CMD_IO;
	memcpy(sqe->cmd, &cmd, sizeof(cmd));
	io_uring_sqe_set_data(sqe, (void *)(uintptr_t)buf_index);
}

int main(int argc, char **argv)
{
	Config cfg = parse_args(argc, argv);

	std::cout << "Configuration:\n"
	          << "  filename:   " << cfg.filename << "\n"
	          << "  type:       " << cfg.type << "\n"
	          << "  size:       " << cfg.size << " bytes\n"
	          << "  iodepth:    " << cfg.iodepth << "\n"
	          << "  block size: " << cfg.block_size << " bytes\n"
	          << "  mode:       " << (cfg.passthrough ? "passthrough" : "direct") << "\n";

	NVMeDevice nvme;
	open_nvme_ssd(cfg.filename, cfg.passthrough, &nvme);

	std::cout << "NVMeDevice:\n"
	          << "  fd:  " << nvme.fd << "\n"
	          << "  nsid:      " << nvme.nsid << "\n"
	          << "  lba_size:   " << nvme.lba_size << " bytes\n"
	          << "  nlba: " << nvme.nlba << "\n";

	struct io_uring ring;
	setup_io_uring(&ring, cfg.iodepth);

	// Allocate IO contexts (buffer + timing info)
	IOContext *io_contexts = new IOContext[cfg.iodepth];
	size_t alignment = nvme.lba_size > 512 ? nvme.lba_size : 512;
	for (int i = 0; i < cfg.iodepth; i++)
	{
		io_contexts[i].buffer = alloc_aligned_buffer(cfg.block_size, alignment);
	}

	// Calculate workload parameters
	uint64_t total_ops = cfg.size / cfg.block_size;
	uint64_t block_lbas = cfg.block_size / nvme.lba_size;
	srand(time(NULL));

	// Latency tracking
	std::vector<double> latencies;
	latencies.reserve(total_ops);

	// Track progress
	uint64_t submitted_ops = 0;
	uint64_t completed_ops = 0;
	int in_flight = 0;

	// Record start time
	TimePoint start_time = Clock::now();

	// Fill queue with initial operations
	while (submitted_ops < total_ops && in_flight < cfg.iodepth)
	{
		uint64_t lba = random_lba(nvme.nlba, block_lbas);
		int buf_idx = in_flight;

		io_contexts[buf_idx].submit_time = Clock::now();

		if (cfg.passthrough)
		{
			submit_read_passthrough(&ring, &nvme, io_contexts[buf_idx].buffer, lba, block_lbas, buf_idx);
		}
		else
		{
			uint64_t offset = lba * nvme.lba_size;
			submit_read_direct(&ring, nvme.fd, io_contexts[buf_idx].buffer, cfg.block_size, offset, buf_idx);
		}

		submitted_ops++;
		in_flight++;
	}

	// Main workload loop
	while (completed_ops < total_ops)
	{
		int ret = io_uring_submit_and_wait(&ring, 1);
		if (ret < 0)
		{
			fatal_error("io_uring_submit_and_wait failed", ret);
		}

		// Process completions
		struct io_uring_cqe *cqe;
		unsigned head;
		unsigned count = 0;

		io_uring_for_each_cqe(&ring, head, cqe)
		{
			if (cqe->res < 0)
			{
				fatal_error("I/O operation failed", cqe->res);
			}

			int buf_idx = (int)(uintptr_t)io_uring_cqe_get_data(cqe);

			// Calculate latency for this operation
			TimePoint complete_time = Clock::now();
			auto duration =
			    std::chrono::duration_cast<std::chrono::nanoseconds>(complete_time - io_contexts[buf_idx].submit_time);
			double latency_us = duration.count() / 1000.0;
			latencies.push_back(latency_us);

			completed_ops++;
			in_flight--;
			count++;

			// Resubmit if more work to do
			if (submitted_ops < total_ops)
			{
				uint64_t lba = random_lba(nvme.nlba, block_lbas);

				io_contexts[buf_idx].submit_time = Clock::now();

				if (cfg.passthrough)
				{
					submit_read_passthrough(&ring, &nvme, io_contexts[buf_idx].buffer, lba, block_lbas, buf_idx);
				}
				else
				{
					uint64_t offset = lba * nvme.lba_size;
					submit_read_direct(&ring, nvme.fd, io_contexts[buf_idx].buffer, cfg.block_size, offset, buf_idx);
				}

				submitted_ops++;
				in_flight++;
			}
		}

		io_uring_cq_advance(&ring, count);
	}

	// Record end time and calculate elapsed
	TimePoint end_time = Clock::now();
	double elapsed_sec = std::chrono::duration<double>(end_time - start_time).count();

	// Print metrics
	print_metrics(latencies, elapsed_sec, completed_ops, cfg.block_size);

	// Free buffers
	for (int i = 0; i < cfg.iodepth; i++)
	{
		free(io_contexts[i].buffer);
	}
	delete[] io_contexts;

	io_uring_queue_exit(&ring);

	close(nvme.fd);
	return 0;
}
