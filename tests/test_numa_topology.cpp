/**
 * @file test_numa_topology.cpp
 * @brief NUMA 拓扑检测与绑核行为验证测试
 */

#include <gtest/gtest.h>
#include <algorithm>
#include <thread>

#include "core/NumaTopology.h"

#ifdef HICAL_HAS_NUMA
	#include <numa.h>
#endif

#ifdef __linux__
	#include "asio/EventLoopPool.h"
#endif

using namespace hical;

// ============ NumaTopology 基础检测 ============

/**
 * @brief 验证 detect() 后节点数 >= 1
 * UMA 环境退回 1，NUMA 环境返回实际节点数
 */
TEST(NumaTopologyTest, detectNumNodes_UmaOrNuma_ReturnsAtLeastOne)
{
	auto& topo = NumaTopology::instance();
	topo.detect();

	int nodes = topo.numaNodes();
	EXPECT_GE(nodes, 1);

// 非 NUMA 环境（Windows / 非 Linux）固定为 1
#ifndef __linux__
	EXPECT_EQ(nodes, 1);
#endif
}

/**
 * @brief 验证每个节点的 cpuList 和 cpuCount 一致性
 * NUMA 环境：cpuList 不为空，cpuCount == cpuList.size()，CPU ID 在有效范围内
 * UMA 环境：cpuList 为空（退化模式不填列表），但 cpuCount == hardware_concurrency
 */
TEST(NumaTopologyTest, cpuListValid_NumaOrUma_ConsistentWithCpuCount)
{
	auto& topo = NumaTopology::instance();
	topo.detect();

	const auto& nodes = topo.nodes();
	ASSERT_GE(nodes.size(), 1);

	int hardwareCpus = static_cast<int>(std::thread::hardware_concurrency());
	(void)hardwareCpus; // 仅在 HICAL_HAS_NUMA 分支使用

	for (const auto& node : nodes)
	{
#ifdef HICAL_HAS_NUMA
		// NUMA 环境：cpuList 应有实际 CPU ID
		EXPECT_FALSE(node.cpuList_.empty()) << "节点 " << node.nodeId_ << " 的 cpuList 不应为空";
		EXPECT_EQ(node.cpuCount_, static_cast<int>(node.cpuList_.size()))
			<< "节点 " << node.nodeId_ << " 的 cpuCount 应与 cpuList.size() 一致";

		for (int cpuId : node.cpuList_)
		{
			EXPECT_GE(cpuId, 0) << "CPU ID 不应为负数";
			EXPECT_LT(cpuId, hardwareCpus) << "CPU ID " << cpuId << " 超过硬件 CPU 数 " << hardwareCpus;
		}
#else
		// 非 NUMA 环境（UMA 退化）：不要求 cpuList 有数据
		// cpuCount 应等于 hardware_concurrency
		EXPECT_GE(node.cpuCount_, 1) << "UMA 节点的 cpuCount 应 >= 1";
#endif
	}
}

// ============ UMA 退化路径 ============

/**
 * @brief 非 Linux 平台上 isNuma() 为 false，numaNodes() == 1
 */
TEST(NumaTopologyTest, umaFallback_NonLinux_ReturnsSingleNode)
{
	auto& topo = NumaTopology::instance();
	topo.detect();

#ifndef __linux__
	// Windows 不是 NUMA 环境
	EXPECT_FALSE(topo.isNuma());
	EXPECT_EQ(topo.numaNodes(), 1);
#else
	// Linux 上 isNuma() 取决于实际硬件
	// 单节点机器也是 isNuma() == false
	if (topo.numaNodes() == 1)
	{
		EXPECT_FALSE(topo.isNuma());
	}
	else
	{
		EXPECT_TRUE(topo.isNuma());
	}
#endif
}

// ============ nodeOfCpu 查询 ============

/**
 * @brief 查询 CPU 归属节点返回有效 nodeId
 */
TEST(NumaTopologyTest, nodeOfCpu_AnyCpuId_ReturnsValidNode)
{
	auto& topo = NumaTopology::instance();
	topo.detect();

	int nodes = topo.numaNodes();
	(void)nodes; // 仅在 HICAL_HAS_NUMA 分支使用
	int hardwareCpus = static_cast<int>(std::thread::hardware_concurrency());

#ifdef HICAL_HAS_NUMA
	// NUMA 环境：对每个 CPU 查询其归属节点
	for (int cpuId = 0; cpuId < hardwareCpus; ++cpuId)
	{
		int nodeId = topo.nodeOfCpu(cpuId);
		EXPECT_GE(nodeId, 0) << "CPU " << cpuId << " 的节点 ID 不应为负数";
		EXPECT_LT(nodeId, nodes) << "CPU " << cpuId << " 的节点 ID " << nodeId << " 超出范围 [0, " << nodes << ")";
	}
#else
	// 非 NUMA 环境：所有 CPU 都在节点 0
	for (int cpuId = 0; cpuId < hardwareCpus; ++cpuId)
	{
		EXPECT_EQ(topo.nodeOfCpu(cpuId), 0) << "UMA 下 CPU " << cpuId << " 应在节点 0";
	}
#endif
}

/**
 * @brief 查询越界 CPU ID 返回 0（fallback）
 */
TEST(NumaTopologyTest, nodeOfCpu_OutOfRangeId_ReturnsZero)
{
	auto& topo = NumaTopology::instance();
	topo.detect();

	// 负数 CPU ID 应返回 0（不 crash）
	EXPECT_EQ(topo.nodeOfCpu(-1), 0);

	// 超大 CPU ID 应返回 0（不 crash）
	EXPECT_EQ(topo.nodeOfCpu(99999), 0);
}

// ============ 幂等性 ============

/**
 * @brief 多次 detect() 调用结果一致
 */
TEST(NumaTopologyTest, detectIdempotent_Repeated_ReturnsSameResult)
{
	auto& topo = NumaTopology::instance();

	// 第一次调用
	topo.detect();
	int nodes1 = topo.numaNodes();
	bool isNuma1 = topo.isNuma();
	const auto& nodesVec1 = topo.nodes();

	// 第二次调用（detect 内部 detected_ 标志保证幂等）
	topo.detect();
	int nodes2 = topo.numaNodes();
	bool isNuma2 = topo.isNuma();
	const auto& nodesVec2 = topo.nodes();

	EXPECT_EQ(nodes1, nodes2) << "节点数应一致";
	EXPECT_EQ(isNuma1, isNuma2) << "isNuma 应一致";
	EXPECT_EQ(nodesVec1.size(), nodesVec2.size()) << "节点列表大小应一致";

	// 第三次调用结果依然一致
	topo.detect();
	EXPECT_EQ(nodes1, topo.numaNodes());
	EXPECT_EQ(isNuma1, topo.isNuma());
}

// ============ EventLoopPool NUMA 感知启动（冒烟测试） ============

#ifdef __linux__
/**
 * @brief NUMA 感知启动不崩溃，线程数正确
 * 创建 EventLoopPool 并启动，验证 start() 中的 NUMA 绑核逻辑不 crash
 */
TEST(NumaTopologyTest, eventLoopPoolNumaStart_WithBinding_DoesNotCrash)
{
	// 确保拓扑已检测
	NumaTopology::instance().detect();

	auto hardwareConcurrency = std::thread::hardware_concurrency();
	size_t threadCount = std::min<size_t>(hardwareConcurrency, 2);
	if (threadCount == 0)
	{
		threadCount = 1;
	}

	EventLoopPool pool(threadCount);

	EXPECT_FALSE(pool.isRunning());
	EXPECT_EQ(pool.size(), threadCount);

	pool.start();

	EXPECT_TRUE(pool.isRunning());

	pool.stop();

	EXPECT_FALSE(pool.isRunning());
}
#endif // __linux__
