/**
 * @file NumaTopology.h
 * @brief NUMA 拓扑检测和查询，封装 libnuma API，非 NUMA 环境自动退化
 */

#pragma once

#include <atomic>
#include <vector>

#ifdef HICAL_HAS_NUMA
	#include <numa.h>
#endif

namespace hical
{

	/**
	 * @brief 单个 NUMA 节点的信息
	 */
	struct NumaNode
	{
		int nodeId_ {0};           /**< NUMA 节点 ID */
		int cpuCount_ {0};         /**< 该节点上的 CPU 数量 */
		std::vector<int> cpuList_; /**< 该节点上的 CPU 编号列表 */
	};

	/**
	 * @brief NUMA 拓扑检测器（单例）
	 * 启动时调用 detect() 完成拓扑检测并缓存结果，之后所有查询都只读已缓存数据。
	 * 非 NUMA 环境（Windows / 找不到 libnuma 的单节点 Linux）自动退化为单节点 UMA，
	 * 所有方法仍然可用，不会产生运行时开销。
	 * 单例模式用 C++11 Magic Statics 保证线程安全，不依赖显式锁。
	 */
	class NumaTopology
	{
	public:
		/**
		 * @brief 获取全局单例
		 * @return NUMA 拓扑检测器引用
		 */
		[[nodiscard]] static NumaTopology& instance();

		/**
		 * @brief 检测 NUMA 拓扑并缓存结果
		 * 启动阶段调用一次（多调也没关系，幂等——只检测一次）。
		 * 非 NUMA 环境会创建 1 个默认 UMA 节点。
		 */
		void detect();

		/**
		 * @brief 是否真正的 NUMA 系统
		 * @return numanodess > 1 时返回 true（必须是多节点 NUMA）
		 */
		[[nodiscard]] bool isNuma() const noexcept;

		/**
		 * @brief 当前配置的 NUMA 节点数
		 * @return 节点数（非 NUMA 环境返回 1——UMA fallback）
		 */
		[[nodiscard]] int numaNodes() const noexcept;

		/**
		 * @brief 查询 CPU 属于哪个 NUMA 节点
		 * @param cpuId CPU 编号
		 * @return 所属 NUMA 节点 ID（非 NUMA 环境始终返回 0）
		 */
		[[nodiscard]] int nodeOfCpu(int cpuId) const noexcept;

		/**
		 * @brief 获取所有节点信息（detect() 后可用）
		 * @return NUMA 节点列表引用
		 */
		[[nodiscard]] const std::vector<NumaNode>& nodes() const noexcept
		{
			return nodes_;
		}

		// 禁止拷贝和移动
		NumaTopology(const NumaTopology&) = delete;
		NumaTopology& operator=(const NumaTopology&) = delete;

	private:
		NumaTopology() = default;
		~NumaTopology() = default;

		/**
		 * @brief 构造 UMA 单节点回退
		 */
		void buildUmaFallback();

		std::atomic<bool> detected_ {false};
		std::vector<NumaNode> nodes_;
	};

} // namespace hical
