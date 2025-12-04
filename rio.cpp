#include <iostream>
#include <cstring>
#include <cstdlib>
#include <getopt.h>

struct Config {
	const char *filename = nullptr;
	const char *type = nullptr;
	size_t size = 0;
	int iodepth = 0;
	size_t block_size = 0;
};

static size_t parse_size(const char *str) {
	char *end;

	size_t val = strtoull(str, &end, 10);

	if (*end == '\0')
		return val;
	switch (*end) {
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

static void usage(const char *prog) {
	std::cerr << "Usage: " << prog << " [options]\n"
	          << "  --filename=<path>  Target device or file\n"
	          << "  --type=<type>      I/O pattern (randomread)\n"
	          << "  --size=<size>      Total workload size (e.g., 1g, 512m)\n"
	          << "  --iodepth=<num>    Queue depth\n"
	          << "  --bs=<size>        Block size (e.g., 4k)\n";
	exit(1);
}

static Config parse_args(int argc, char **argv) {
	Config cfg;

	static struct option long_options[] = {
	    {"filename", required_argument, 0, 'f'}, {"type", required_argument, 0, 't'},
	    {"size", required_argument, 0, 's'},     {"iodepth", required_argument, 0, 'd'},
	    {"bs", required_argument, 0, 'b'},       {0, 0, 0, 0}};

	int opt;
	while ((opt = getopt_long(argc, argv, "", long_options, nullptr)) != -1) {
		switch (opt) {
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
		default:
			usage(argv[0]);
		}
	}

	if (!cfg.filename || !cfg.type || cfg.size == 0 || cfg.iodepth == 0 || cfg.block_size == 0) {
		std::cerr << "Error: All parameters are required\n";
		usage(argv[0]);
	}

	if (strcmp(cfg.type, "randomread") != 0) {
		std::cerr << "Error: Only 'randomread' type is supported\n";
		exit(1);
	}

	return cfg;
}

int main(int argc, char **argv) {
	Config cfg = parse_args(argc, argv);

	std::cout << "Configuration:\n"
	          << "  filename:  " << cfg.filename << "\n"
	          << "  type:      " << cfg.type << "\n"
	          << "  size:      " << cfg.size << " bytes\n"
	          << "  iodepth:   " << cfg.iodepth << "\n"
	          << "  block size: " << cfg.block_size << " bytes\n";

	return 0;
}
