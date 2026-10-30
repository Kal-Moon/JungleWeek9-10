#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "threads/interrupt.h"
#include "threads/io.h"
#include "threads/synch.h"
#include "threads/thread.h"

/* See [8254] for hardware details of the 8254 timer chip. */

#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

/* Number of timer ticks since OS booted. */
static int64_t ticks;

/* Number of loops per timer tick.
   Initialized by timer_calibrate(). */
static unsigned loops_per_tick;

static intr_handler_func timer_interrupt;
static bool too_many_loops (unsigned loops);
static void busy_wait (int64_t loops);
static void real_time_sleep (int64_t num, int32_t denom);
/* timer_sleep()으로 잠든 스레드 하나를 나타낸다.
   timer_sleep()의 지역 변수로 만들어지며, 스레드가 sema_down()에서
   잠들어 있는 동안에는 함수가 끝나지 않으므로 스택 위에서 안전하게 유지된다. */
struct sleeper {
	int64_t wakeup;              /* 깨어날 절대 시각 (부팅 후 tick 수). */
	struct semaphore sema;       /* 0으로 초기화. down으로 재우고 up으로 깨운다. */
	struct list_elem elem;       /* sleep_list에 연결하기 위한 원소. */
};

/* 잠든 스레드들의 목록. wakeup이 이른 순서로 정렬되어 있다.
   timer_sleep()(스레드)과 timer_interrupt()(인터럽트)가 함께 접근하므로
   스레드 쪽에서는 인터럽트를 끈 상태에서만 수정한다. */
static struct list sleep_list;

/* Sets up the 8254 Programmable Interval Timer (PIT) to
   interrupt PIT_FREQ times per second, and registers the
   corresponding interrupt. */
void
timer_init (void) {
	/* 8254 input frequency divided by TIMER_FREQ, rounded to
	   nearest. */
	uint16_t count = (1193180 + TIMER_FREQ / 2) / TIMER_FREQ;

	outb (0x43, 0x34);    /* CW: counter 0, LSB then MSB, mode 2, binary. */
	outb (0x40, count & 0xff);
	outb (0x40, count >> 8);

	/* 잠든 스레드 목록 초기화. 타이머 인터럽트가 등록되기 전에 해야 한다. */
	list_init (&sleep_list);
	intr_register_ext (0x20, timer_interrupt, "8254 Timer");
}

/* Calibrates loops_per_tick, used to implement brief delays. */
void
timer_calibrate (void) {
	unsigned high_bit, test_bit;

	ASSERT (intr_get_level () == INTR_ON);
	printf ("Calibrating timer...  ");

	/* Approximate loops_per_tick as the largest power-of-two
	   still less than one timer tick. */
	loops_per_tick = 1u << 10;
	while (!too_many_loops (loops_per_tick << 1)) {
		loops_per_tick <<= 1;
		ASSERT (loops_per_tick != 0);
	}

	/* Refine the next 8 bits of loops_per_tick. */
	high_bit = loops_per_tick;
	for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
		if (!too_many_loops (high_bit | test_bit))
			loops_per_tick |= test_bit;

	printf ("%'"PRIu64" loops/s.\n", (uint64_t) loops_per_tick * TIMER_FREQ);
}

/* Returns the number of timer ticks since the OS booted. */
int64_t
timer_ticks (void) {
	enum intr_level old_level = intr_disable ();
	int64_t t = ticks;
	intr_set_level (old_level);
	barrier ();
	return t;
}

/* Returns the number of timer ticks elapsed since THEN, which
   should be a value once returned by timer_ticks(). */
int64_t
timer_elapsed (int64_t then) {
	return timer_ticks () - then;
}

/* sleep_list 정렬용 비교 함수.
   A가 B보다 먼저 깨어나야 하면 true를 반환한다.
   같은 시각이면 false이므로, 먼저 잠든 스레드가 앞에 남는다. */
static bool
wakeup_less (const struct list_elem *a, const struct list_elem *b,
             void *aux UNUSED) {
	const struct sleeper *sa = list_entry (a, struct sleeper, elem);
	const struct sleeper *sb = list_entry (b, struct sleeper, elem);
	return sa->wakeup < sb->wakeup;
}

/* Suspends execution for approximately TICKS timer ticks.

   busy wait(thread_yield 반복) 대신, 현재 스레드를 sleep_list에 넣고
   세마포어로 BLOCKED 상태로 만든다. 깰 시각이 되면 timer_interrupt()가
   sema_up()으로 READY 상태로 돌려놓는다.
   주의: 인자 TICKS가 전역 변수 ticks를 가린다. 여기서 ticks는 "잘 시간"이다. */
void
timer_sleep (int64_t ticks) {
	int64_t start = timer_ticks ();

	ASSERT (intr_get_level () == INTR_ON);
	if (ticks <= 0)          /* 잘 필요 없음 (alarm-zero, alarm-negative). */
		return;

	struct sleeper s;
	s.wakeup = start + ticks;          /* 상대 시간을 절대 시각으로 변환. */
	sema_init (&s.sema, 0);

	/* 삽입 도중 타이머 인터럽트가 sleep_list를 건드리지 못하게 막는다. */
	enum intr_level old_level = intr_disable ();
	list_insert_ordered (&sleep_list, &s.elem, wakeup_less, NULL);
	intr_set_level (old_level);

	/* 값이 0이므로 여기서 BLOCKED. timer_interrupt()의 sema_up()까지 대기. */
	sema_down (&s.sema);
}

/* Suspends execution for approximately MS milliseconds. */
void
timer_msleep (int64_t ms) {
	real_time_sleep (ms, 1000);
}

/* Suspends execution for approximately US microseconds. */
void
timer_usleep (int64_t us) {
	real_time_sleep (us, 1000 * 1000);
}

/* Suspends execution for approximately NS nanoseconds. */
void
timer_nsleep (int64_t ns) {
	real_time_sleep (ns, 1000 * 1000 * 1000);
}

/* Prints timer statistics. */
void
timer_print_stats (void) {
	printf ("Timer: %"PRId64" ticks\n", timer_ticks ());
}

/* Timer interrupt handler.
   매 tick마다 sleep_list 앞쪽에서 깰 시각이 된 스레드들을 깨운다.
   리스트가 wakeup 순으로 정렬되어 있으므로, 아직 깰 시각이 아닌 원소를
   만나면 뒤쪽은 볼 필요 없이 멈춘다. 인터럽트 컨텍스트이므로
   이미 인터럽트가 꺼져 있고, 잠들 수 있는 sema_down()은 쓰면 안 된다. */
static void
timer_interrupt (struct intr_frame *args UNUSED) {
	ticks++;
	thread_tick ();

	while (!list_empty (&sleep_list)) {
		struct sleeper *s = list_entry (list_front (&sleep_list), struct sleeper, elem);
		if (s->wakeup > ticks)
			break;
		/* 먼저 리스트에서 빼고 깨운다. 깨어난 스레드가 timer_sleep()을
		   빠져나가면 s(스택 메모리)가 사라지기 때문이다. */
		list_pop_front (&sleep_list);
		sema_up (&s->sema);
	}
}

/* Returns true if LOOPS iterations waits for more than one timer
   tick, otherwise false. */
static bool
too_many_loops (unsigned loops) {
	/* Wait for a timer tick. */
	int64_t start = ticks;
	while (ticks == start)
		barrier ();

	/* Run LOOPS loops. */
	start = ticks;
	busy_wait (loops);

	/* If the tick count changed, we iterated too long. */
	barrier ();
	return start != ticks;
}

/* Iterates through a simple loop LOOPS times, for implementing
   brief delays.

   Marked NO_INLINE because code alignment can significantly
   affect timings, so that if this function was inlined
   differently in different places the results would be difficult
   to predict. */
static void NO_INLINE
busy_wait (int64_t loops) {
	while (loops-- > 0)
		barrier ();
}

/* Sleep for approximately NUM/DENOM seconds. */
static void
real_time_sleep (int64_t num, int32_t denom) {
	/* Convert NUM/DENOM seconds into timer ticks, rounding down.

	   (NUM / DENOM) s
	   ---------------------- = NUM * TIMER_FREQ / DENOM ticks.
	   1 s / TIMER_FREQ ticks
	   */
	int64_t ticks = num * TIMER_FREQ / denom;

	ASSERT (intr_get_level () == INTR_ON);
	if (ticks > 0) {
		/* We're waiting for at least one full timer tick.  Use
		   timer_sleep() because it will yield the CPU to other
		   processes. */
		timer_sleep (ticks);
	} else {
		/* Otherwise, use a busy-wait loop for more accurate
		   sub-tick timing.  We scale the numerator and denominator
		   down by 1000 to avoid the possibility of overflow. */
		ASSERT (denom % 1000 == 0);
		busy_wait (loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000));
	}
}
