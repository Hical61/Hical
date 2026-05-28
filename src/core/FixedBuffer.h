/**
 * @file FixedBuffer.h
 * @brief 栈分配定长缓冲区（堆回退）
 */

#pragma once

#include <charconv>
#include <cstring>
#include <string>
#include <string_view>

namespace hical
{

	/**
	 * @brief 栈上固定缓冲区，用于高性能日志格式化
	 * 正常日志（< N 字节）零堆分配；溢出时自动 fallback 到 std::string。
	 * 整数/浮点格式化使用 std::to_chars 直写栈缓冲，避免 locale 开销。
	 * @tparam N 栈缓冲区大小（默认 4096 字节）
	 */
	template <size_t N = 4096>
	class FixedBuffer
	{
	public:
		FixedBuffer() = default;
		FixedBuffer(const FixedBuffer&) = delete;
		FixedBuffer& operator=(const FixedBuffer&) = delete;
		FixedBuffer(FixedBuffer&&) noexcept = default;
		FixedBuffer& operator=(FixedBuffer&&) noexcept = default;
		~FixedBuffer() = default;

		void append(const char* data, size_t len)
		{
			if (!overflowed_)
			{
				if (used_ + len <= N)
				{
					std::memcpy(stackBuf_ + used_, data, len);
					used_ += len;
					return;
				}
				// 溢出：拷贝已有数据到 heap
				heapBuf_.assign(stackBuf_, used_);
				overflowed_ = true;
			}
			heapBuf_.append(data, len);
		}

		void append(std::string_view sv)
		{
			append(sv.data(), sv.size());
		}

		FixedBuffer& operator<<(std::string_view sv)
		{
			append(sv.data(), sv.size());
			return *this;
		}

		FixedBuffer& operator<<(const char* s)
		{
			if (s != nullptr)
			{
				append(s, std::strlen(s));
			}
			return *this;
		}

		FixedBuffer& operator<<(char c)
		{
			append(&c, 1);
			return *this;
		}

		FixedBuffer& operator<<(bool val)
		{
			return val ? (*this << std::string_view("true")) : (*this << std::string_view("false"));
		}

		FixedBuffer& operator<<(int val)
		{
			return formatInteger(val);
		}

		FixedBuffer& operator<<(unsigned val)
		{
			return formatInteger(val);
		}

		FixedBuffer& operator<<(long val)
		{
			return formatInteger(val);
		}

		FixedBuffer& operator<<(unsigned long val)
		{
			return formatInteger(val);
		}

		FixedBuffer& operator<<(long long val)
		{
			return formatInteger(val);
		}

		FixedBuffer& operator<<(unsigned long long val)
		{
			return formatInteger(val);
		}

		FixedBuffer& operator<<(double val)
		{
			return formatFloat(val);
		}

		FixedBuffer& operator<<(float val)
		{
			return formatFloat(static_cast<double>(val));
		}

		[[nodiscard]] std::string_view view() const
		{
			if (overflowed_)
			{
				return heapBuf_;
			}
			return {stackBuf_, used_};
		}

		[[nodiscard]] const char* data() const
		{
			return overflowed_ ? heapBuf_.data() : stackBuf_;
		}

		[[nodiscard]] size_t size() const
		{
			return overflowed_ ? heapBuf_.size() : used_;
		}

		[[nodiscard]] bool overflowed() const
		{
			return overflowed_;
		}

		[[nodiscard]] size_t capacity() const
		{
			return N;
		}

		void clear()
		{
			used_ = 0;
			overflowed_ = false;
			heapBuf_.clear();
		}

	private:
		static constexpr size_t kNumericBufSize = 32;

		template <typename T>
		FixedBuffer& formatInteger(T val)
		{
			char tmp[kNumericBufSize];
			auto [ptr, ec] = std::to_chars(tmp, tmp + kNumericBufSize, val);
			if (ec == std::errc {})
			{
				append(tmp, static_cast<size_t>(ptr - tmp));
			}
			return *this;
		}

		FixedBuffer& formatFloat(double val)
		{
			char tmp[kNumericBufSize];
			auto [ptr, ec] = std::to_chars(tmp, tmp + kNumericBufSize, val, std::chars_format::general);
			if (ec == std::errc {})
			{
				append(tmp, static_cast<size_t>(ptr - tmp));
			}
			return *this;
		}

		char stackBuf_[N] {};
		size_t used_ {0};
		bool overflowed_ {false};
		std::string heapBuf_;
	};

} // namespace hical
