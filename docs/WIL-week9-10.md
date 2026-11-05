# [크래프톤 정글] WIL 9~10주차 — PintOS Project 1 (Threads)

> 기간: 9~10주차 (2026.10.27 ~ 11.04)
> 주제: PintOS Threads — Alarm Clock, Priority Scheduling, Priority Donation / Project 2 (User Program) 진입

---

## 이번 주에 한 일 한눈에 보기

| 과제 | 핵심 변경 | 상태 |
|------|-----------|------|
| Alarm Clock | busy wait → `sleep_list` + 세마포어로 재우고 깨우기 | 구현 |
| Priority Scheduling | 선점, `sema_up`/`cond_signal`에서 최고 우선순위 깨우기 | 구현 |
| Priority Donation | 단일·다중·연쇄(nested) 기부, `set_priority` 복원 | 구현 |
| Advanced Scheduler (MLFQS) | — | 미구현 |
| Project 2 진입 | Argument Passing, SPT 기초 (팀 통합) | 진행 중 |

---

## 1. Alarm Clock — "기다리는 것도 CPU를 쓴다"

### 공부한 것

원래 `timer_sleep()`은 이렇게 생겼다.

```c
while (timer_elapsed (start) < ticks)
    thread_yield ();
```

잠자는 스레드가 계속 ready_list로 돌아와서 "아직이야?"를 물어보는 **busy waiting**이다. 자는 동안에도 스케줄링 대상이 되니 CPU를 계속 쓴다.

바꾼 방식은 **잘 스레드를 BLOCKED로 만들고, 깰 시각이 되면 타이머 인터럽트가 깨워주는 것**이다.

- `struct sleeper { wakeup; sema; elem; }`를 `timer_sleep()`의 **지역 변수(스택)** 로 만든다.
- 상대 시간 `ticks`를 절대 시각 `start + ticks`로 바꿔서 `sleep_list`에 **wakeup 순으로 정렬 삽입**한다.
- 값이 0인 세마포어에 `sema_down()` → BLOCKED.
- `timer_interrupt()`가 매 tick마다 리스트 앞쪽만 보고, 깰 시각이 된 것들을 `sema_up()`.
  정렬돼 있으니 아직 깰 때가 아닌 원소를 만나면 바로 `break`.

`thread_block()`/`thread_unblock()`을 직접 쓰지 않고 **세마포어로 재우고 깨운** 게 포인트였다. 세마포어가 대기열 관리와 block/unblock을 대신 해 주니까 코드가 훨씬 단순해졌다.

### 어려웠던 것

- **인터럽트 컨텍스트에서 할 수 있는 일과 없는 일.**
  `timer_interrupt()`는 인터럽트 핸들러라서 잠들 수 있는 함수(`sema_down`, `lock_acquire`)를 부르면 안 된다. `sema_up`은 되고 `sema_down`은 안 되는 이유가 "잠들 수 있느냐"라는 걸 이해하는 데 시간이 걸렸다.
- **`sleep_list`를 누가 건드리나.**
  스레드(`timer_sleep`)와 인터럽트(`timer_interrupt`)가 같은 리스트에 접근한다. 락은 인터럽트 핸들러에서 못 쓰니 **스레드 쪽에서 삽입할 때 인터럽트를 끄는 것**이 유일한 동기화 수단이었다.
- **스택 메모리의 수명.**
  `sleeper`가 지역 변수라서, 깨어난 스레드가 `timer_sleep()`을 빠져나가는 순간 사라진다. 그래서 인터럽트 핸들러에서 **먼저 `list_pop_front()`로 빼고 나서 `sema_up()`** 해야 한다. 순서를 반대로 하면 이미 사라진 메모리를 리스트가 가리킬 수 있다.
- `timer_sleep(int64_t ticks)`의 인자 `ticks`가 전역 변수 `ticks`를 가린다(shadowing). 처음에 이걸 몰라서 헷갈렸다.
- `ticks <= 0`이면 바로 리턴해야 `alarm-zero`, `alarm-negative`를 통과한다.

---

## 2. Priority Scheduling — "정렬해 둘까, 꺼낼 때 찾을까"

### 공부한 것

목표는 **항상 가장 높은 우선순위의 스레드가 CPU를 잡는 것**.

1. ready_list에서 최고 우선순위를 꺼낸다 (`next_thread_to_run`).
2. 더 높은 스레드가 ready가 되면 **즉시 선점**한다 (`thread_preempt`).
3. 세마포어/조건변수에서 깨울 때도 **대기자 중 최고 우선순위**를 깨운다.

`thread_preempt()`는 `thread_create()`, `sema_up()`, `thread_set_priority()` 끝에서 호출했다.

```c
void thread_preempt (void) {
    ...
    should_yield = top->priority > thread_current ()->priority;
    ...
    if (should_yield) {
        if (intr_context ()) intr_yield_on_return ();
        else                 thread_yield ();
    }
}
```

### 어려웠던 것

- **A 방식(정렬 삽입) vs B 방식(꺼낼 때 `list_max`).**
  처음엔 Alarm Clock 커밋에서 `list_insert_ordered()`로 ready_list를 정렬해 넣었다. 그런데 Donation까지 생각하면 **리스트에 들어간 뒤에 우선순위가 바뀐다.** 정렬 상태가 깨지면 맨 앞이 최댓값이라는 보장이 사라진다.
  그래서 **넣을 때는 `push_back`, 꺼낼 때 `list_max()`로 찾는 B 방식**으로 갈아탔다. O(n)이지만 항상 정확하다.
- **`list_max()`의 비교 함수 방향.**
  `list_max()`는 `less(max, e)`가 true일 때만 max를 바꾼다. 그래서 비교 함수는 "A < B"여야 하고, 같은 우선순위면 앞쪽(먼저 온) 스레드가 뽑혀 **FIFO / 라운드 로빈이 유지된다.** `>`로 쓰면 최솟값을 뽑는다는 걸 한참 뒤에 알았다.
- **선점 조건은 `>=`가 아니라 `>`.** 같은 우선순위끼리 서로 양보하면 불필요한 컨텍스트 스위칭이 생긴다.
- **인터럽트 안에서는 `thread_yield()`를 못 한다.** `sema_up()`은 인터럽트 핸들러(`timer_interrupt`)에서도 불리기 때문에 `intr_context()`면 `intr_yield_on_return()`으로 "핸들러 끝나면 양보해"라고 표시만 해야 한다.
- **`sema_up()`에서 `value++`의 위치.** 양보하기 **전에** 값을 올려야, 깨어난 스레드가 `sema_down()`의 `while (value == 0)`을 통과할 수 있다.
- **조건변수(`cond_signal`)가 특히 헷갈렸다.**
  `cond->waiters`에는 스레드가 아니라 `semaphore_elem`이 들어 있다. 처음엔 각 `semaphore_elem` 안의 `semaphore.waiters`를 들여다보려 했는데, `cond_wait()`에서 `sema_down()` 하기 전에 signal이 오면 그 리스트가 비어 있을 수 있다.
  그래서 `semaphore_elem`에 **`struct thread *thread`를 직접 저장**했다. 포인터로 저장했기 때문에 비교 시점의 (기부까지 반영된) 현재 우선순위를 읽는다.

---

## 3. Priority Donation — 이번 주차 최대 난관

### 공부한 것: Priority Inversion

```
L(낮음)이 락을 잡음 → H(높음)가 그 락을 기다림 → M(중간)이 계속 실행
→ L이 CPU를 못 받아서 락을 못 풀고, H는 M보다 높은데도 무한 대기
```

해결책은 **H가 L에게 자기 우선순위를 빌려주는 것(donation)**. L이 락을 풀면 원래대로 돌아간다.

세 가지 경우를 모두 처리해야 했다.

- **단일 기부**: H → L
- **다중 기부**: L이 락 A, B를 쥐고 있고 각각 다른 스레드가 기부 → 락 하나를 풀면 **그 락 관련 기부만** 빠져야 함
- **연쇄 기부(nested)**: H → M → L. M이 L의 락을 기다리는 중이면 H의 우선순위가 L까지 전달돼야 함

### 설계

`struct thread`에 네 필드를 추가했다.

```c
int base_priority;              /* 스스로 정한 원래 우선순위 */
struct list donation;           /* 나에게 기부한 스레드들 (받는 쪽) */
struct list_elem donation_elem; /* 남의 donation 줄에 걸리는 고리 (주는 쪽) */
struct lock *wait_lock;         /* 지금 기다리는 락 */
```

- `priority` = 기부까지 반영된 값 (스케줄러는 이것만 본다)
- `base_priority` = `thread_set_priority()`로만 바뀌는 값
- 재계산 규칙: **`priority = max(base_priority, donation 줄의 최댓값)`**

흐름은 이렇다.

- `lock_acquire()`: holder가 있으면 `wait_lock` 기록 → holder의 `donation`에 줄 서기 → `wait_lock`을 따라가며 연쇄 기부 → `sema_down()`
- `lock_release()`: `donation`에서 `wait_lock == lock`인 스레드만 제거 → `thread_recompute_priority()` → `sema_up()`
- `thread_set_priority()`: `base_priority`만 바꾸고 재계산

### 어려웠던 것

- **`elem`과 `donation_elem`을 따로 둬야 하는 이유.**
  기부하는 스레드는 동시에 락의 `semaphore.waiters`에도 들어가 있다. 이때 이미 `elem`을 쓰고 있으니, donation 줄에 걸 고리가 따로 필요하다. **하나의 `list_elem`은 한 번에 한 리스트에만 들어갈 수 있다**는 걸 몸으로 배웠다.
- **`list_entry()`의 멤버 이름을 잘못 쓰면 엉뚱한 메모리를 읽는다.**
  donation 줄을 순회할 때 `list_entry(e, struct thread, elem)`으로 쓰면 오프셋이 달라서 쓰레기 값을 스레드로 읽는다. 그래서 `priority_compare`와 거의 똑같은 `compare_donation_priority`를 **`donation_elem` 기준으로 따로** 만들었다. 컴파일러가 잡아주지 않는 버그라 무서웠다.
- **기부는 `sema_down()` 앞에서.** `sema_down()` 안에 들어가면 BLOCKED라서 아무것도 할 수 없다.
- **연쇄 기부를 어떻게 표현할까.**
  줄(`donation` 리스트)에는 **바로 앞 holder에게만** 서고, 그 뒤로는 `wait_lock->holder`를 따라가며 값만 올렸다. 뒤쪽 holder의 donation 줄엔 앞쪽 holder가 이미 서 있으니, 재계산할 때 그 앞쪽 holder의 "올라간" priority를 읽게 되어 연쇄가 자연스럽게 유지된다.
- **락을 얻은 뒤 `wait_lock = NULL`.** 안 지우면 다음 연쇄 기부 때 이미 끝난 기다림을 따라간다.
- **`lock_release()`의 순서.** `thread_recompute_priority()`를 `sema_up()`보다 **먼저** 해야 한다. `sema_up()` 안의 `thread_preempt()`가 **내려간** 우선순위로 비교해야 깨어난 대기자에게 양보하기 때문이다.
- **리스트 순회 중 삭제.** `list_remove(e)` 후의 `e->next`는 믿을 수 없어서 `e = list_remove(e)`의 반환값으로 다음으로 넘어가야 한다.
- **초기화 함정 두 가지.**
  - `init_thread()`에서 `base_priority`를 안 넣으면 `memset`의 0으로 남아서, 기부가 끝난 순간 우선순위가 0으로 떨어진다.
  - `struct list`는 0으로 채워져도 빈 리스트가 아니다. `list_init()`을 꼭 불러야 한다 (head/tail 센티널이 서로를 가리켜야 함).
- **`thread_set_priority()`가 기부를 날려버리는 문제.** `priority`에 바로 대입하면 받고 있던 기부가 사라진다. `base_priority`만 바꾸고 재계산하도록 고쳤다.
- **동기화.** holder 확인부터 기부까지 사이에 선점되면 holder가 바뀔 수 있어서, `lock_acquire()` / `lock_release()` 전체를 인터럽트 비활성화로 감쌌다.

---

## 4. Project 2 진입 — User Program 맛보기

10주차 후반에는 팀에서 Project 2로 넘어가며 **Argument Passing**과 VM용 **Supplemental Page Table(SPT)** 기초를 통합했다.

### 공부한 것

- 커맨드라인 `"echo x y"`를 `strtok_r()`로 잘라 `argv[]`를 만든다.
- 유저 스택에 **문자열 → 패딩(16바이트 정렬) → `argv[argc] = NULL` → `argv[]` 포인터들 → 가짜 return address** 순으로 쌓는다.
- x86-64 호출 규약에 따라 `rdi = argc`, `rsi = argv`.
- `_start` 진입 시점에 `rsp % 16 == 8` (일반 `call` 직후와 같은 상태)이어야 한다.

### 어려웠던 것

- 스택이 **아래로 자란다**는 것과, 문자열을 복사한 뒤 `argv[i]`를 **커널 주소가 아닌 유저 스택 주소**로 바꿔 넣어야 한다는 점.
- 정렬 패딩 계산과, 한 페이지(4KB) 안에 다 들어가는지 **쓰기 전에 먼저 검사**하는 경계 처리.
- QEMU 실행 환경 문제로 공식 `args-*` 테스트를 아직 직접 돌려 보지 못했다. 다음 주에 꼭 확인해야 한다.

---

## 회고

### 잘한 점
- 단계별로 PR을 나눠(Alarm → Priority → Donation) 리뷰받고 머지했다.
- 코드에 "왜 이렇게 했는지"를 주석으로 남겨서, 나중에 다시 봤을 때 이해가 빨랐다.
- 정렬 삽입 방식이 Donation과 맞지 않는다는 걸 깨닫고 **설계를 갈아엎는 결정**을 했다.

### 아쉬운 점
- MLFQS(Advanced Scheduler)를 구현하지 못했다. 고정소수점 연산(17.14)과 `recent_cpu`, `load_avg` 공식은 따로 공부해 둘 것.
- (직접 느낀 아쉬운 점을 추가하기)

### 다음 주 목표
- [ ] Project 2: System Call (`exit`, `write`, `open`, `read`, `fork`, `exec`, `wait`) 구현
- [ ] 유저 메모리 접근 검증 (bad pointer 테스트)
- [ ] `args-*` 테스트 직접 실행해서 PASS 확인

### 한 줄 정리
> **"리스트에 들어간 뒤에도 값이 바뀔 수 있다면, 정렬을 믿지 말고 꺼낼 때 찾아라."**
> 그리고 인터럽트 컨텍스트, 스택 메모리 수명, `list_elem` 하나는 한 리스트에만 — 이 세 가지가 이번 주차 내내 발목을 잡았다.
