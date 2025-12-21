#pragma once

#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace graphics {
namespace vulkan {

// Minimal move-only type erasure for deferred release callbacks (C++17-friendly).
class MoveOnlyFunction {
  public:
	MoveOnlyFunction() = default;

	template <typename F>
	explicit MoveOnlyFunction(F&& f)
	    : m_fn(std::make_unique<Model<std::decay_t<F>>>(std::forward<F>(f)))
	{
	}

	MoveOnlyFunction(const MoveOnlyFunction&) = delete;
	MoveOnlyFunction& operator=(const MoveOnlyFunction&) = delete;

	MoveOnlyFunction(MoveOnlyFunction&&) noexcept = default;
	MoveOnlyFunction& operator=(MoveOnlyFunction&&) noexcept = default;

	explicit operator bool() const { return static_cast<bool>(m_fn); }

	void operator()() noexcept
	{
		if (m_fn) {
			m_fn->call();
		}
	}

	void reset() noexcept { m_fn.reset(); }

  private:
	struct Concept {
		virtual ~Concept() = default;
		virtual void call() noexcept = 0;
	};

	template <typename F>
	struct Model final : Concept {
		F fn;
		explicit Model(F&& f) : fn(std::move(f)) {}
		void call() noexcept override { fn(); }
	};

	std::unique_ptr<Concept> m_fn;
};

// Serial-gated deferred destruction queue used to make GPU lifetime explicit.
class DeferredReleaseQueue {
  public:
	struct Entry {
		uint64_t retireSerial = 0;
		MoveOnlyFunction release;
	};

	template <typename F>
	void enqueue(uint64_t retireSerial, F&& releaseFn)
	{
		Entry e;
		e.retireSerial = retireSerial;
		e.release = MoveOnlyFunction(std::forward<F>(releaseFn));
		m_entries.push_back(std::move(e));
	}

	void collect(uint64_t completedSerial)
	{
		size_t writeIdx = 0;
		for (auto& e : m_entries) {
			if (e.retireSerial <= completedSerial) {
				e.release();
			} else {
				m_entries[writeIdx++] = std::move(e);
			}
		}
		m_entries.resize(writeIdx);
	}

	void clear() noexcept
	{
		for (auto& e : m_entries) {
			e.release();
		}
		m_entries.clear();
	}

	size_t size() const { return m_entries.size(); }

  private:
	std::vector<Entry> m_entries;
};

} // namespace vulkan
} // namespace graphics
