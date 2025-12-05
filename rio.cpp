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

int main(int argc, char **argv)
{
	Config cfg = parse_args(argc, argv);

	std::cout << "Configuration:\n"
	          << "  filename:  " << cfg.filename << "\n"
	          << "  type:      " << cfg.type << "\n"
	          << "  size:      " << cfg.size << " bytes\n"
	          << "  iodepth:   " << cfg.iodepth << "\n"
	          << "  block size: " << cfg.block_size << " bytes\n";

	NVMeDevice nvme;
	open_nvme_ssd(cfg.filename, cfg.passthrough, &nvme);

	std::cout << "NVMeDevice:\n"
	          << "  fd:  " << nvme.fd << "\n"
	          << "  nsid:      " << nvme.nsid << "\n"
	          << "  lba_size:   " << nvme.lba_size << " bytes\n"
	          << "  nlba: " << nvme.nlba << "\n";

	struct io_uring ring;
	setup_io_uring(&ring, cfg.iodepth);
	// do io_uring something
	io_uring_queue_exit(&ring);

	close(nvme.fd);
	return 0;
}
