#include "api_overflow_buffer.h"
#ifdef USE_API
#include <array>
#include <cstring>

#include <compile_time_ut.hpp>

namespace esphome::api {

// Pure index/copy arithmetic for APIOverflowBuffer, kept side-effect-free so its
// invariants are asserted at compile time, directly below each definition: they
// run on every build of this file, and a violation is a compile error with the
// offending values in the diagnostic — no test harness or runtime cost involved.
namespace overflow_math {

/// One iov's contribution to an enqueue copy that skips the first `skip` bytes of
/// the overall iov sequence and writes into a `dst_size`-byte destination at `dst_pos`.
struct CopyStep {
  uint16_t src_offset;  // where to start reading inside this iov
  uint16_t copy_len;    // bytes to copy from this iov (0 = iov fully skipped)
  uint16_t next_skip;   // skip remaining for the following iov
};

constexpr CopyStep plan_copy_step(size_t iov_len, uint16_t skip, uint16_t dst_pos, uint16_t dst_size) {
  if (skip >= iov_len) {
    return {0, 0, static_cast<uint16_t>(skip - iov_len)};
  }
  uint16_t len = static_cast<uint16_t>(iov_len - skip);
  // Never write past the destination: if the caller's total_len disagrees with the
  // actual iov lengths, truncate the frame instead of corrupting the heap.
  const uint16_t space = dst_pos < dst_size ? static_cast<uint16_t>(dst_size - dst_pos) : 0;
  if (len > space)
    len = space;
  return {skip, len, 0};
}

constexpr bool test_plan_copy_step() {
  using namespace CompileTimeUnitTesting;
  // Whole iov fits: copy everything from offset 0.
  expect_eq<plan_copy_step(10, 0, 0, 10).src_offset, 0>();
  expect_eq<plan_copy_step(10, 0, 0, 10).copy_len, 10>();
  expect_eq<plan_copy_step(10, 0, 0, 10).next_skip, 0>();
  // Skip lands inside this iov: copy its tail.
  expect_eq<plan_copy_step(10, 4, 0, 6).src_offset, 4>();
  expect_eq<plan_copy_step(10, 4, 0, 6).copy_len, 6>();
  // Skip covers this whole iov (boundary case skip == iov_len included):
  // nothing copied, remainder of the skip carries to the next iov.
  expect_eq<plan_copy_step(10, 14, 0, 100).copy_len, 0>();
  expect_eq<plan_copy_step(10, 14, 0, 100).next_skip, 4>();
  expect_eq<plan_copy_step(10, 10, 0, 100).copy_len, 0>();
  expect_eq<plan_copy_step(10, 10, 0, 100).next_skip, 0>();
  // A caller total_len/iov mismatch must clamp, never overrun the destination
  // (the heap-corruption class behind #18434).
  expect_eq<plan_copy_step(10, 0, 4, 7).copy_len, 3>();
  expect_eq<plan_copy_step(10, 0, 7, 7).copy_len, 0>();  // destination already full
  expect_eq<plan_copy_step(10, 0, 9, 7).copy_len, 0>();  // dst_pos past dst_size
  return true;
}
static_assert(test_plan_copy_step());

// Drive the exact enqueue copy loop over an iov-length sequence and return the
// final write position — proving the loop never writes past dst_size.
constexpr uint16_t simulate_enqueue(const std::array<size_t, 4> &iov_lens, uint16_t skip, uint16_t dst_size) {
  uint16_t to_skip = skip;
  uint16_t write_pos = 0;
  for (size_t len : iov_lens) {
    const CopyStep step = plan_copy_step(len, to_skip, write_pos, dst_size);
    write_pos += step.copy_len;
    to_skip = step.next_skip;
  }
  return write_pos;
}

constexpr std::array<size_t, 4> TEST_IOV_LENS{3, 7, 2, 5};  // sums to 17

constexpr bool test_simulate_enqueue() {
  using namespace CompileTimeUnitTesting;
  // Consistent caller (total 17): buffer fills exactly.
  expect_eq<simulate_enqueue(TEST_IOV_LENS, 0, 17), 17>();
  // Skip spanning the first two iovs: 17-8 = 9 bytes land.
  expect_eq<simulate_enqueue(TEST_IOV_LENS, 8, 9), 9>();
  // Skip exactly on an iov boundary.
  expect_eq<simulate_enqueue(TEST_IOV_LENS, 10, 7), 7>();
  // Inconsistent caller (iov sum 17, destination only 5): writes stop at 5.
  expect_eq<simulate_enqueue(TEST_IOV_LENS, 0, 5), 5>();
  expect_eq<simulate_enqueue(TEST_IOV_LENS, 4, 2), 2>();
  return true;
}
static_assert(test_simulate_enqueue());

/// Advance a ring-buffer index with wraparound.
constexpr uint8_t ring_next(uint8_t index, uint8_t capacity) { return static_cast<uint8_t>((index + 1) % capacity); }

/// Step a ring-buffer index backwards with wraparound.
constexpr uint8_t ring_prev(uint8_t index, uint8_t capacity) {
  return static_cast<uint8_t>((index + capacity - 1) % capacity);
}

constexpr bool test_ring_index() {
  using namespace CompileTimeUnitTesting;
  expect_eq<ring_next(0, 8), 1>();
  expect_eq<ring_next(7, 8), 0>();
  expect_eq<ring_prev(0, 8), 7>();
  expect_eq<ring_prev(1, 8), 0>();
  return true;
}
static_assert(test_ring_index());

constexpr bool test_ring_prev_inverts_next() {
  for (uint8_t i = 0; i < API_MAX_SEND_QUEUE; i++) {
    if (ring_prev(ring_next(i, API_MAX_SEND_QUEUE), API_MAX_SEND_QUEUE) != i)
      return false;
    if (ring_next(ring_prev(i, API_MAX_SEND_QUEUE), API_MAX_SEND_QUEUE) != i)
      return false;
  }
  return true;
}
static_assert(test_ring_prev_inverts_next());

/// Where a new entry goes and how head/tail move.
struct RingPush {
  uint8_t slot;  // queue index that receives the new entry
  uint8_t head;  // updated head
  uint8_t tail;  // updated tail
};

/// A partial remainder (front == true) is pushed at the FRONT of the ring: its
/// first bytes are already on the wire, so it must drain before any frame a
/// re-entrant send enqueued while the partial write was in progress.
constexpr RingPush ring_push(uint8_t head, uint8_t tail, bool front, uint8_t capacity) {
  if (front) {
    const uint8_t slot = ring_prev(head, capacity);
    return {slot, slot, tail};
  }
  return {tail, head, ring_next(tail, capacity)};
}

constexpr bool test_ring_push() {
  using namespace CompileTimeUnitTesting;
  // Back push: entry lands on the old tail, head untouched.
  expect_eq<ring_push(2, 5, false, 8).slot, 5>();
  expect_eq<ring_push(2, 5, false, 8).head, 2>();
  expect_eq<ring_push(2, 5, false, 8).tail, 6>();
  expect_eq<ring_push(2, 7, false, 8).tail, 0>();  // tail wraps
  // Front push: entry lands just before the old head, tail untouched.
  expect_eq<ring_push(2, 5, true, 8).slot, 1>();
  expect_eq<ring_push(2, 5, true, 8).head, 1>();
  expect_eq<ring_push(2, 5, true, 8).tail, 5>();
  expect_eq<ring_push(0, 5, true, 8).slot, 7>();  // head wraps
  // The ordering invariant of the #18434 fix: a front push is the next entry
  // drained (slot == new head), a back push is not (slot != head when occupied).
  expect_true<(ring_push(3, 6, true, 8).slot == ring_push(3, 6, true, 8).head)>();
  expect_true<(ring_push(3, 6, false, 8).slot != ring_push(3, 6, false, 8).head)>();
  return true;
}
static_assert(test_ring_push());

}  // namespace overflow_math

APIOverflowBuffer::~APIOverflowBuffer() {
  for (auto *entry : this->queue_) {
    if (entry != nullptr)
      Entry::destroy(entry);
  }
}

ssize_t APIOverflowBuffer::try_drain(socket::Socket *socket) {
  // socket->write() can re-enter this function: a log message emitted from an
  // lwip callback during the write goes out over the API and lands back in the
  // frame helper's write/drain path. If a nested drain ran here it would send
  // and free the entry the outer drain is still holding, causing a double free.
  // Report "no progress" instead; the outer drain keeps draining, and the
  // nested send is enqueued behind the existing backlog.
  if (this->draining_)
    return 0;

  // RAII so the flag is cleared on every return path
  struct DrainGuard {
    explicit DrainGuard(bool &flag) : flag_(flag) { flag_ = true; }
    ~DrainGuard() { this->flag_ = false; }
    bool &flag_;
  } guard(this->draining_);

  while (this->count_ > 0) {
    Entry *front = this->queue_[this->head_];

    ssize_t sent = socket->write(front->current_data(), front->remaining());

    if (sent <= 0) {
      // -1 = error (caller checks errno for EWOULDBLOCK vs hard errors)
      // 0 = nothing sent (treat as no progress)
      return sent;
    }

    if (static_cast<uint16_t>(sent) < front->remaining()) {
      // Partially sent, update offset and stop
      front->offset += static_cast<uint16_t>(sent);
      return sent;
    }

    // Entry fully sent — unlink it before freeing so a freed pointer is never
    // reachable from the queue
    this->queue_[this->head_] = nullptr;
    this->head_ = overflow_math::ring_next(this->head_, API_MAX_SEND_QUEUE);
    this->count_--;
    Entry::destroy(front);
  }

  return 0;  // All drained
}

bool APIOverflowBuffer::enqueue_iov(const struct iovec *iov, int iovcnt, uint16_t total_len, uint16_t skip) {
  if (this->count_ >= API_MAX_SEND_QUEUE)
    return false;
  if (skip >= total_len)
    return true;  // Nothing left to queue

  uint16_t buffer_size = total_len - skip;
  // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
  auto *entry = new Entry{new uint8_t[buffer_size], buffer_size, 0};

  uint16_t to_skip = skip;
  uint16_t write_pos = 0;

  for (int i = 0; i < iovcnt; i++) {
    const auto step = overflow_math::plan_copy_step(iov[i].iov_len, to_skip, write_pos, buffer_size);
    if (step.copy_len > 0) {
      std::memcpy(entry->data + write_pos, reinterpret_cast<uint8_t *>(iov[i].iov_base) + step.src_offset,
                  step.copy_len);
      write_pos += step.copy_len;
    }
    to_skip = step.next_skip;
  }

  // A partial remainder (skip > 0) goes to the FRONT of the ring — see ring_push().
  const auto pos = overflow_math::ring_push(this->head_, this->tail_, skip > 0, API_MAX_SEND_QUEUE);
  this->queue_[pos.slot] = entry;
  this->head_ = pos.head;
  this->tail_ = pos.tail;
  this->count_++;
  return true;
}

}  // namespace esphome::api

#endif  // USE_API
