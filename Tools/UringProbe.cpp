// Probes the io_uring surface gyro actually asks for, using raw syscalls so the answer is the
// kernel's rather than liburing's. Reports per ring configuration, never per kernel version:
// distributions backport flags into older kernels and io_uring_disabled subtracts them from newer
// ones, so a version test errs in both directions. See Docs/Decisions.md decision 3.
//
// This is the first draft of gyro's own startup probe. It is standalone so it can be run on a
// target machine to answer "will gyro start here" before gyro exists there.

#include <linux/io_uring.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace
{

int Setup(unsigned entries, io_uring_params& params)
{
	return static_cast<int>(syscall(__NR_io_uring_setup, entries, &params));
}

int Register(int ring, unsigned op, void* arg, unsigned nrArgs)
{
	return static_cast<int>(syscall(__NR_io_uring_register, ring, op, arg, nrArgs));
}

struct Configuration
{
	const char* Name;
	unsigned Flags;
	const char* Wanted;
};

struct Feature
{
	unsigned Bit;
	const char* Name;
};

struct Opcode
{
	unsigned Op;
	const char* Name;
	const char* For;
};

void TryConfiguration(const Configuration& configuration)
{
	io_uring_params params = {};
	params.flags = configuration.Flags;

	const int ring = Setup(64, params);
	if (ring < 0)
	{
		printf("  %-28s FAIL  errno=%d (%s)\n", configuration.Name, errno, strerror(errno));
		printf("  %-28s       wanted: %s\n", "", configuration.Wanted);
		return;
	}

	printf("  %-28s OK    features=0x%08x\n", configuration.Name, params.features);
	close(ring);
}

} // namespace

int main()
{
	utsname system = {};
	uname(&system);
	printf("kernel  %s\n", system.release);

	if (FILE* sysctl = fopen("/proc/sys/kernel/io_uring_disabled", "r"))
	{
		int value = -1;
		if (fscanf(sysctl, "%d", &value) == 1)
			printf("io_uring_disabled  %d  (0=enabled, 1=privileged only, 2=disabled)\n", value);
		fclose(sysctl);
	}
	else
	{
		printf("io_uring_disabled  absent (pre-6.6 kernel, or io_uring not compiled in)\n");
	}

	printf("\nRing configurations gyro asks for:\n");

	static const Configuration configurations[] = {
		{ "plain", 0, "nothing - baseline" },
		{ "SINGLE_ISSUER", IORING_SETUP_SINGLE_ISSUER, "one submitting thread per ring" },
		{ "SINGLE_ISSUER|DEFER_TASKRUN",
		  IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN,
		  "the frame ring - completion work where we ask for it" },
		{ "DEFER_TASKRUN alone", IORING_SETUP_DEFER_TASKRUN, "expected to fail: DEFER_TASKRUN requires SINGLE_ISSUER" },
	};

	for (const Configuration& configuration : configurations)
		TryConfiguration(configuration);

	io_uring_params params = {};
	params.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;

	const int ring = Setup(64, params);
	if (ring < 0)
	{
		printf("\nFrame-ring configuration unavailable: errno=%d (%s).\n", errno, strerror(errno));
		printf("gyro would refuse to start here; there is no epoll fallback by design.\n");
		return 1;
	}

	static const Feature features[] = {
		{ IORING_FEAT_SINGLE_MMAP, "SINGLE_MMAP" },
		{ IORING_FEAT_NODROP, "NODROP" },
		{ IORING_FEAT_SUBMIT_STABLE, "SUBMIT_STABLE" },
		{ IORING_FEAT_RW_CUR_POS, "RW_CUR_POS" },
		{ IORING_FEAT_FAST_POLL, "FAST_POLL" },
		{ IORING_FEAT_POLL_32BITS, "POLL_32BITS" },
		{ IORING_FEAT_EXT_ARG, "EXT_ARG" },
		{ IORING_FEAT_NATIVE_WORKERS, "NATIVE_WORKERS" },
		{ IORING_FEAT_REG_REG_RING, "REG_REG_RING" },
		{ IORING_FEAT_MIN_TIMEOUT, "MIN_TIMEOUT" },
	};

	printf("\nFeatures on the frame ring (! prefix means absent):\n ");
	for (const Feature& feature : features)
		printf(" %s%s", (params.features & feature.Bit) ? "" : "!", feature.Name);
	printf("\n");

	static const Opcode opcodes[] = {
		{ IORING_OP_TIMEOUT, "TIMEOUT", "frame ring: absolute deadline wakeup" },
		{ IORING_OP_TIMEOUT_REMOVE, "TIMEOUT_REMOVE", "frame ring: rearm on clock invalidation" },
		{ IORING_OP_POLL_ADD, "POLL_ADD", "frame ring: DRM fd; dispatch ring: every fd" },
		{ IORING_OP_POLL_REMOVE, "POLL_REMOVE", "both rings" },
		{ IORING_OP_READ, "READ", "frame ring: DRM event read" },
		{ IORING_OP_ACCEPT, "ACCEPT", "dispatch ring: the control socket" },
		{ IORING_OP_RECVMSG, "RECVMSG", "not used - libwayland owns the client sockets" },
	};

	unsigned char probeStorage[sizeof(io_uring_probe) + 256 * sizeof(io_uring_probe_op)] = {};
	io_uring_probe* probe = reinterpret_cast<io_uring_probe*>(probeStorage);

	if (Register(ring, IORING_REGISTER_PROBE, probe, 256) < 0)
	{
		printf("\nIORING_REGISTER_PROBE failed: errno=%d (%s)\n", errno, strerror(errno));
		close(ring);
		return 1;
	}

	printf("\nOpcodes (last supported = %u):\n", probe->last_op);
	for (const Opcode& opcode : opcodes)
	{
		const bool supported = opcode.Op <= probe->last_op && (probe->ops[opcode.Op].flags & IO_URING_OP_SUPPORTED);
		printf("  %-16s %-4s  %s\n", opcode.Name, supported ? "yes" : "NO", opcode.For);
	}

	// The frame loop's timeout is absolute and in CLOCK_MONOTONIC, which is decision 57's timebase.
	// Monotonic is io_uring's default; BOOTTIME and REALTIME are opt-in flags, and decision 57
	// refuses both.
	printf("\nTimeout flags in the uapi header:\n");
#ifdef IORING_TIMEOUT_ABS
	printf("  IORING_TIMEOUT_ABS        yes  - absolute deadline, the frame loop's form\n");
#else
	printf("  IORING_TIMEOUT_ABS        NO\n");
#endif
#ifdef IORING_TIMEOUT_BOOTTIME
	printf("  IORING_TIMEOUT_BOOTTIME   yes  - refused by decision 57\n");
#endif
#ifdef IORING_TIMEOUT_MULTISHOT
	printf("  IORING_TIMEOUT_MULTISHOT  yes\n");
#endif

	close(ring);
	return 0;
}
