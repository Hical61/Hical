/**
 * @file NumaTopology.cpp
 * @brief NUMA 拓扑检测和查询实现
 */

#include "NumaTopology.h"
#include "Log.h"

#include <algorithm>
#include <thread>

namespace hical
{

	NumaTopology& NumaTopology::instance()
	{
		static NumaTopology s_instance;
		return s_instance;
	}

	void NumaTopology::buildUmaFallback()
	{
		nodes_.clear();
		NumaNode umaNode;
		umaNode.nodeId_ = 0;
		umaNode.cpuCount_ = static_cast<int>(std::thread::hardware_concurrency());
		// cpuList_ 不填（UMA 场景不需要逐 CPU 查询节点）
		nodes_.push_back(std::move(umaNode));
	}

	void NumaTopology::detect()
	{
		// exchange 保证多线程并发 detect() 只有一个执行检测
		if (detected_.exchange(true, std::memory_order_acquire))
		{
			return;
		}

#ifdef HICAL_HAS_NUMA
		// numa_available() 返回 0 表示 NUMA 可用，-1 表示不可用
		if (numa_available() == -1)
		{
			HICAL_LOG_INFO("NUMA 不可用，退化为 1 个 UMA 节点");
			buildUmaFallback();
			return;
		}

		int maxNode = numa_max_node();
		int totalCpus = numa_num_configured_cpus();

		if (maxNode < 0 || totalCpus <= 0)
		{
			HICAL_LOG_INFO("NUMA 拓扑检测失败（maxNode={}, totalCpus={}），退化为 UMA", maxNode, totalCpus);
			buildUmaFallback();
			return;
		}

		// maxNode 是最大节点 ID（从 0 开始），实际节点数 = maxNode + 1
		// 但有些系统节点 ID 可能不连续，所以先收集每节点的 CPU 列表
		int nodeCount = maxNode + 1;
		nodes_.resize(nodeCount);
		for (int i = 0; i < nodeCount; ++i)
		{
			nodes_[i].nodeId_ = i;
		}

		// 遍历所有 CPU，归入对应节点
		for (int cpu = 0; cpu < totalCpus; ++cpu)
		{
			int node = numa_node_of_cpu(cpu);
			if (node >= 0 && node < static_cast<int>(nodes_.size()))
			{
				nodes_[node].cpuList_.push_back(cpu);
			}
		}

		// 统计每节点 CPU 数，并移除没有 CPU 的节点（sparse node ID）
		std::vector<NumaNode> compactedNodes;
		for (auto& node : nodes_)
		{
			if (node.cpuList_.empty())
			{
				continue; // 跳过空节点（sparse node ID）
			}
			node.cpuCount_ = static_cast<int>(node.cpuList_.size());
			compactedNodes.push_back(std::move(node));
		}

		if (compactedNodes.empty())
		{
			HICAL_LOG_INFO("NUMA 拓扑检测结果为空，退化为 UMA");
			buildUmaFallback();
			return;
		}

		nodes_ = std::move(compactedNodes);

		HICAL_LOG_INFO("NUMA 拓扑检测完成：{} 个节点，{} 个 CPU", nodes_.size(), totalCpus);
		return;
#else
		HICAL_LOG_DEBUG("非 NUMA 环境，使用 UMA 单节点退化");
		buildUmaFallback();
#endif
	}

	bool NumaTopology::isNuma() const noexcept
	{
		return nodes_.size() > 1;
	}

	int NumaTopology::numaNodes() const noexcept
	{
		return static_cast<int>(nodes_.size());
	}

	int NumaTopology::nodeOfCpu(int cpuId) const noexcept
	{
		// UMA 退化：所有 CPU 都在节点 0
		if (nodes_.size() <= 1)
		{
			return 0;
		}

		// 遍历各节点查找 CPU
		for (const auto& node : nodes_)
		{
			if (std::find(node.cpuList_.begin(), node.cpuList_.end(), cpuId) != node.cpuList_.end())
			{
				return node.nodeId_;
			}
		}

		// 没找到，返回 0 作为 fallback
		return 0;
	}

} // namespace hical
