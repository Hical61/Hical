/**
 * @file HttpConnectionRegistry.h
 * @brief HTTP 会话的关闭与排空控制登记（框架内部）
 */

#pragma once

#ifdef HICAL_HAS_HTTP2

	#include <boost/asio/any_io_executor.hpp>
	#include <cstddef>
	#include <functional>
	#include <memory>

namespace hical::detail
{
	/**
	 * @brief 独立于空闲扫描器的连接控制表，广播持有强引用快照
	 * 表结构由互斥锁保护；回调、注销及回调替换只在连接所属的事件循环执行。
	 * 调用方须让 executor 的 io_context 覆盖登记和所有已投递操作的生命周期。
	 */
	class HttpConnectionRegistry
	{
		struct State;
		struct Control;

	public:
		using Callback = std::function<void()>;

		/**
		 * @brief 会话持有的唯一登记守卫，必须先于被回调引用的 socket 析构
		 */
		class Registration
		{
		public:
			Registration() = default;
			~Registration();
			Registration(const Registration&) = delete;
			Registration& operator=(const Registration&) = delete;
			Registration(Registration&& other) noexcept;
			Registration& operator=(Registration&& other) noexcept;

			/**
			 * @brief 在所属事件循环注销，并让已排队的通知失效；重复调用无副作用
			 */
			void reset() noexcept;

			/**
			 * @brief 握手后接入会话排空入口；此前已收到的排空通知会立即补发一次
			 * @param callback 所属事件循环内调用的排空回调
			 */
			void setDrainHandler(Callback callback);

			/**
			 * @brief 查询是否已收到排空通知，仅在所属事件循环使用
			 * @return true 表示已经要求排空
			 */
			[[nodiscard]] bool draining() const noexcept;

		private:
			friend class HttpConnectionRegistry;
			Registration(std::shared_ptr<State> state, std::shared_ptr<Control> control);
			std::shared_ptr<State> state_;
			std::shared_ptr<Control> control_;
		};

		HttpConnectionRegistry();
		~HttpConnectionRegistry();
		HttpConnectionRegistry(const HttpConnectionRegistry&) = delete;
		HttpConnectionRegistry& operator=(const HttpConnectionRegistry&) = delete;

		/**
		 * @brief 在首次握手挂起前登记会话
		 * @param executor socket 所属的事件循环执行器
		 * @param close 关闭底层 socket 的回调，不得为空
		 * @return 唯一登记守卫；回调引用的对象必须覆盖该守卫的生命周期
		 */
		[[nodiscard]] Registration add(boost::asio::any_io_executor executor, Callback close);

		/**
		 * @brief 可跨线程广播关闭，回调始终在各连接所属的事件循环执行
		 */
		void requestClose();

		/**
		 * @brief 可跨线程广播排空，不阻止随后登记；晚到会话由 server 状态复查覆盖
		 */
		void requestDrain();

		/**
		 * @brief 查询仍登记的连接数，允许跨线程调用
		 * @return 当前登记数量
		 */
		[[nodiscard]] size_t size() const;

	private:
		void notify(bool close);
		std::shared_ptr<State> state_;
	};
} // namespace hical::detail

#endif
