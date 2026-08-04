/* Linux */
/* x86_64 */

/* Audio droupouts are likely to happen on a weak PC! */
/* swapoff -a is recommended */

#if !defined(CACHE_LINE)
#define CACHE_LINE 64
#endif

/* TODO: AArch64 */

#define MINIAUDIO_IMPLEMENTATION
#define MA_ENABLE_ONLY_SPECIFIC_BACKENDS
#define MA_ENABLE_ALSA
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MA_NO_WAV
#define MA_NO_FLAC
#define MA_NO_MP3
#define MA_NO_GENERATION
#define MA_API static

#include "miniaudio.h"
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#if !defined(min)
#define min(x, y) ((x) < (y) ? (x) : (y))
#endif

#if !defined(max)
#define max(x, y) ((x) > (y) ? (x) : (y))
#endif

static float minf(float x, float y)
{
	return min(x, y);
}

static float maxf(float x, float y)
{
	return max(x, y);
}

#if !defined(M_PIf)
#define M_PIf 3.14159265358979323846f
#endif

#if !defined(M_PI_2f)
#define M_PI_2f 1.57079632679489661923f
#endif

#if !defined(M_PI_4f)
#define M_PI_4f 0.78539816339744830962f
#endif

#if !defined(M_Ef)
#define M_Ef 2.7182818284590452354f
#endif

#define asm __asm__

static void store_fence(void)
{
	asm volatile("sfence" ::: "memory");
}

static void load_fence(void)
{
	asm volatile("lfence" ::: "memory");
}

static void full_memory_fence(void)
{
	asm volatile("mfence" ::: "memory");
}

static void compiler_memory_fence(void)
{
	asm volatile("" ::: "memory");
}

static unsigned long get_cpu_ticks(void)
{
#if 1
	unsigned long rax;
	unsigned long rdx;

	/* volatile is required */
	asm volatile("lfence\nrdtsc" : "=a"(rax), "=d"(rdx) : :);
	return rdx << 32 | rax;
#else
	/* no lfence */
	return __rdtsc();
#endif
}

struct data {
	_Alignas(CACHE_LINE) unsigned long nframes; /* atomic */

	/* atomic. least 40: tick_begin, most 24: nticks */
	_Alignas(CACHE_LINE) unsigned long tick_begin_nticks;

	_Alignas(CACHE_LINE) int warmup_counter;       /* callback-private */
	_Alignas(8) /*    */ unsigned long tick_start; /* read-only */
};

__attribute__((noinline)) void rt_audio_callback(ma_device *restrict p_device, void *restrict p_output,
						 const void *restrict p_input, ma_uint32 frame_count)
{
	float *output;
	const float *input;
	struct data *restrict p_shared_data;
	unsigned long nframes;
	unsigned long tick_begin;
	unsigned long tick_end;
	unsigned long i;
	unsigned long tmp;
	float x;

	tick_begin = get_cpu_ticks();

	p_shared_data = p_device->pUserData;
	if (__builtin_expect(p_shared_data == NULL, 0)) {
		return;
	}

	output = p_output;
	input = p_input;

	nframes = __atomic_load_n(&p_shared_data->nframes, __ATOMIC_ACQUIRE);

	for (i = 0; i < frame_count; ++i) {
		tmp = nframes + i;
		if (tmp & 0x8000000000000000) {
			/* (2^63-1)/44100/42/60/60/24/366 > 100000 years */
			__builtin_unreachable();
		}
		x = tmp / 44100.f * 2 * M_PIf * 440;
		x = fmodf(x, 2 * M_PIf);
		output[i * 2 + 0] = minf(maxf(sinf(x) * 0.5f + input[i], -1), 1);
		output[i * 2 + 1] = output[i * 2 + 0];
	}

	if (__builtin_expect(p_shared_data->warmup_counter != 0, 0)) {
		--p_shared_data->warmup_counter;
		if (p_shared_data->warmup_counter == 0) {
			__atomic_store_n(&p_shared_data->nframes, 0, __ATOMIC_RELEASE);
		}

		compiler_memory_fence();
		for (i = 0; i < frame_count * 2; ++i) {
			output[i] = 0;
		}
	}

	/* this callback function is not static and therefore 'called in unpredictable way'. No -flto! */
	/* CPU reordering acquire with release? */
	/* x86_64 and AArch64 */
	__atomic_store_n(&p_shared_data->nframes, nframes + frame_count, __ATOMIC_RELEASE);
	
	tick_end = get_cpu_ticks();
	__atomic_store_n(&p_shared_data->tick_begin_nticks,
			 tick_end - tick_begin << 40 | tick_begin - p_shared_data->tick_start,
			 __ATOMIC_RELEASE);
}

int main(void)
{
	ma_device_config deviceConfig;
	ma_device device;
	struct data shared_data;
	unsigned long nframes;
	unsigned long tmp;
	unsigned long prev;
	unsigned long prev_nfr;
	unsigned long i;

	int device_init;
	int mem_lock;

	device_init = 0;
	mem_lock = 0;

	deviceConfig = ma_device_config_init(ma_device_type_playback | ma_device_type_capture);

	deviceConfig.playback.format = ma_format_f32;
	deviceConfig.playback.channels = 2;

	deviceConfig.capture.format = ma_format_f32;
	deviceConfig.capture.channels = 1;

	deviceConfig.sampleRate = 44100;
	deviceConfig.dataCallback = rt_audio_callback;
	deviceConfig.pUserData = &shared_data;
	deviceConfig.periods = 1;		    /* 3 */
	deviceConfig.periodSizeInMilliseconds = 10; /* For pure loopback, 3 ms is enough on a powerful PC */

	if (MA_SUCCESS != ma_device_init(NULL, &deviceConfig, &device)) {
		printf("Failed to open device.\n");
		goto l_failed;
	}
	device_init = 1;

	/* would be better with MCL_CURRENT */
	if (0 != mlockall(MCL_FUTURE)) {
		printf("mlockall failed\n");
		goto l_failed;
	}
	mem_lock = 1;

	shared_data.tick_start = get_cpu_ticks();
	shared_data.warmup_counter = 64;
	__atomic_store_n(&shared_data.nframes, 1000000, __ATOMIC_RELAXED);
	
	store_fence();
	if (MA_SUCCESS != ma_device_start(&device)) {
		printf("Failed to start playback device.\n");
		goto l_failed;
	}

	nframes = __atomic_load_n(&shared_data.nframes, __ATOMIC_ACQUIRE);
	tmp = __atomic_load_n(&shared_data.tick_begin_nticks, __ATOMIC_ACQUIRE);
	prev = tmp & 0x000000FFffffFFFF;
	prev_nfr = nframes;

	for (i = 0; i < 1000; ++i) {
		usleep(10000);
		nframes = __atomic_load_n(&shared_data.nframes, __ATOMIC_ACQUIRE);
		tmp = __atomic_load_n(&shared_data.tick_begin_nticks, __ATOMIC_ACQUIRE);
		printf("%lu %lu %lu\n", nframes - prev_nfr, (tmp & 0x000000FFffffFFFF) - prev, tmp >> 40);
		prev = tmp & 0x000000FFffffFFFF;
		prev_nfr = nframes;
	}

	printf("Press Enter to quit...");
	getchar();

	full_memory_fence();

l_failed:

	if (device_init)
		ma_device_uninit(&device);

	if (mem_lock)
		munlockall();

	return 0;
}

