/**
 * 成员反射收集函数组总览
 *
 * data_members_of(class_type_info, ctx)
 *      获取当前类直接全部数据成员：静态+非静态；不递归继承链，不做二义过滤。
 *
 * check_syntax_valid(class_type_info, member_info)
 *      独立consteval校验函数；借助元模板CheckTemplate + meta::can_substitute；
 *      检测 obj.member 对象直接访问语法是否良构，识别菱形非虚继承子对象多实例二义。
 *
 * collect_nonambiguous_member_infos_template(class_type_info, ctx, members_of_func)
 *      DFS遍历继承链，模拟C++名字隐藏，产出 AmbiguousMap（名字→候选元信息集合）；
 *      仅做meta::info去重；**不做语法良构校验，无法识别菱形非虚继承子对象多实例二义**。
 *
 * collect_nonambiguous_member_infos_coarse_filtered(class_type_info, ctx, members_of_func)
 *      粗过滤层：基于AmbiguousMap，仅保留候选集合size==1的成员；
 *      只筛名字层面继承二义；依然无法识别菱形非虚继承子对象二义。
 *
 * collect_static_member_infos(class_type_info, ctx)
 *      静态成员专用：直接使用粗过滤；静态成员无实例子对象，不需要语法良构校验。
 *
 * collect_nonstatic_member_infos(class_type_info, ctx)
 *      仅非静态数据成员完整收集入口：粗过滤 + check_syntax_valid语法校验流式组合；
 *      返回真正可以通过对象`.`直接访问的非静态数据成员。
 *
 * collect_member_infos(class_type_info, ctx)
 *      全部数据成员（静态+非静态）统一入口；粗过滤名字二义；
 *      非静态成员额外做对象访问语法校验；静态成员跳过语法校验直接放行。
 *
 * collect_member_infos_simple_template(class_type_info, ctx, members_of_func)
 *      最简单实现版本：递归收集当前类型及其全部基类的成员反射元信息
 *
 */
#pragma once
#include <flat_map>
#include <meta>
#include <ranges>
#include <string_view>
#include <vector>

namespace hical::CollectMember {
    namespace M = std::meta;
    namespace R = std::ranges;
    namespace V = R::views;
    /**
     * @brief 获取类全部数据成员：静态数据成员 + 非静态数据成员
     * @param class_type_info 目标类的类型反射元信息
     * @param ctx 反射访问上下文，控制访问权限过滤
     * @return std::vector<meta::info> 合并后的静态、非静态数据成员元信息列表
     * @note
     *  简单拼接 meta::static_data_members_of 与 meta::nonstatic_data_members_of 的返回结果；
     *  不做继承链递归、不做名字二义过滤，仅返回当前类直接定义的数据成员。
     */
    consteval auto data_members_of(const M::info class_type_info, const M::access_context ctx) -> std::vector<M::info> {
        std::vector<M::info> result;
        result.append_range(M::static_data_members_of(class_type_info, ctx));
        result.append_range(M::nonstatic_data_members_of(class_type_info, ctx));
        return result;
    }

    /**
     * @brief 成员信息提取可调用对象概念约束
     * @tparam Fn 待约束的可调用对象类型，可以是仿函数、函数指针、lambda、静态成员函数类型。
     * @note 调用签名：std::invoke(Fn, meta::info{}, meta::access_context{}) → std::vector<meta::info>
     */
    template <typename Fn>
    concept MemberInfoGatherer =
        std::invocable<Fn, M::info, M::access_context> &&
        std::same_as<std::invoke_result_t<Fn, M::info, M::access_context>, std::vector<M::info>>;

    /**
     * @brief 编译期元模板：用于实例替换校验成员对象访问是否良构
     * @tparam ClassType 目标类类型（通过反射元替换注入）
     * @tparam member_info 待检测成员的反射元信息常量
     * @note 该模板本身不做业务逻辑，仅作为 substitution‑context 上下文；
     *       内层requires约束会在实例替换时触发语法合法性诊断；
     *       配合 meta::can_substitute 判断 std::declval<ClassType>().[:member_info:] 对象成员访问表达式是否良构。
     */
    template <typename ClassType, M::info member_info>
    requires requires { std::declval<ClassType>().[:member_info:]; }
    struct CheckTemplate{};

    /**
     * @brief 检查类对象直接访问该成员是否为C++良构语法
     * @param class_type_info 目标类的类型反射元信息
     * @param member_info 待校验成员的反射元信息
     * @return true：obj.member 形式对象访问语法合法；false：语法非法（如菱形非虚继承子对象多实例二义等）
     * @note 执行原理：
     *  1. 借助 meta::can_substitute 尝试实例化 CheckTemplate；
     *     若模板实例化满足requires条件则返回true，替换失败返回false；
     *  2. 校验语义等价于：`std::declval<T>().member;` 对象`.`直接访问语法；
     * @warning 警告相关：
     *  1. **不校验类限定名访问 `T::member`**，只检测实例对象访问；
     *  2. 访问权限已经由上层 meta::access_context 过滤，本函数不再重复做权限检查；
     *  3. 不要用于静态成员校验：静态成员使用对象`.`访问本身属于C++允许但不推荐语法，本校验结果无参考意义；
     *  4. 依赖编译器反射扩展特性 meta::can_substitute、元模板实例替换机制。
     */
    consteval bool check_syntax_valid(const M::info class_type_info, const M::info member_info) {
        return M::can_substitute(^^CheckTemplate, { class_type_info, M::reflect_constant(member_info) });
    }

    /**
     * @brief 同名字成员候选映射：key为成员标识符，value为该名字下全部候选元信息集合
     * @note vector.size() > 1 代表该名字存在继承二义；派生类同名成员会替换整个候选集合实现名字隐藏
     */
    using AmbiguousMap = std::flat_map<std::string_view, std::vector<M::info>>;

    /**
     * @brief 递归遍历类继承链，收集所有数据成员元信息，模拟C++派生类名字隐藏规则
     * @param class_type_info 目标类的类型反射元信息
     * @param ctx 反射访问上下文，控制访问权限过滤
     * @param members_of_func 反射成员查询的函数
     * @return AmbiguousMap 成员名到候选元信息集合的映射；
     *          vector长度大于1：仅来自多个基类重名、当前类没有同名覆盖，该名字依然存在二义。
     * @warning
     *  该函数仅对**反射元信息 meta::info 对象做去重**，当你把函数用于非静态成员收集时，不会模拟C++中非虚菱形继承的语法二义：
     *  当菱形继承未使用 virtual 继承修饰中层基类时，虽然来自顶层公共基类的成员元信息在输出vector内仅存唯一一份，
     *  但C++语法层面派生类会拥有多份顶层基类子对象，直接访问该成员名字依然属于二义访问，编译报错。
     *  本函数无法识别此种场景，不会将该名字标记为二义，需要上层调用方额外做子对象二义性校验。
     *
     * @note 执行原理：
     *  1. DFS深度优先：先递归解析所有直接基类，合并基类成员候选集合；
     *  2. 合并基类结果时对 meta::info 做去重，避免继承链重复引入同一成员；
     *  3. 再处理当前类自身非静态数据成员：
     *     - 若当前类存在与基类重名的成员，直接替换候选集合；遵循C++名字查找规则：派生类名字隐藏基类同名符号，消除该名字的继承二义。
     *     - 如果候选集没有此字段名，则创建新的键值对。
     */
    consteval auto collect_nonambiguous_member_infos_template(
        const M::info class_type_info, const M::access_context ctx, const decltype(M::members_of) members_of_func
    ) -> AmbiguousMap {

        /// key:成员名字标识符；value:该名字对应的全部候选成员元信息集合
        AmbiguousMap name_to_member_candidates;

        // DFS深度优先解析所有直接基类，收集并合并基类的成员候选集合
        const auto direct_base_info_list = M::bases_of(class_type_info, ctx);
        for (const M::info single_base_info : direct_base_info_list) {

            // 递归获取该基类完整的【名字→候选成员】映射
            const AmbiguousMap base_name_candidate_map =
                collect_nonambiguous_member_infos_template(M::type_of(single_base_info), ctx, members_of_func);

            // 将基类候选集合合并入当前类的候选映射
            for (const auto& [candidate_member_name, base_same_name_candidates] : base_name_candidate_map) {

                // 把 base_same_name_candidates 元素并入 merged_same_name_candidates，做去重，避免同一个 meta::info 重复存放
                auto& merged_same_name_candidates = name_to_member_candidates[candidate_member_name];
                for (const M::info candidate_member_info : base_same_name_candidates) {

                    // 判断 merged_same_name_candidates 中是否已经存在该元信息
                    const bool exists = R::any_of(merged_same_name_candidates, [&candidate_member_info](const M::info item) {
                        return item == candidate_member_info;
                    });
                    if (!exists) {
                        merged_same_name_candidates.push_back(candidate_member_info);
                    }
                }
            }
        }

        // 处理当前类自身的非静态数据成员
        const auto current_class_members = members_of_func(class_type_info, ctx);
        for (const auto single_member_info : current_class_members) {
            // 过滤匿名位域等无标识符的成员元信息
            if (M::has_identifier(single_member_info)) {
                const std::string_view current_member_identifier = M::identifier_of(single_member_info);

                /**
                 * 1. 当前类出现与基类重名成员时，直接整体替换候选集合；派生类名字屏蔽全部基类同名符号，消解该名字的继承二义。
                 * 2. 如果候选集没有此字段名，则创建新的键值对。
                 * 以上条件执行本质都是覆写操作。
                 */
                name_to_member_candidates[current_member_identifier] = { single_member_info };
            }
        }

        return name_to_member_candidates;
    }

    /**
     * @brief 粗过滤获取无继承名字二义的数据成员元信息列表
     * @param class_type_info 目标类的类型反射元信息
     * @param ctx 反射访问上下文，控制访问权限过滤
     * @param members_of_func 反射成员查询的函数
     * @return std::vector<meta::info> 仅保留名字不存在继承二义的成员元信息；
     *          仅保留候选集合size == 1的成员：代表该名字下仅有唯一有效候选，不存在多基类重名二义，且未被派生类名字隐藏。
     *
     * @warning 警告相关：
     *  1. 本层只做**名字层面的二义粗筛**，依然无法识别菱形非虚继承带来的子对象多实例二义；
     *     即使成员通过本过滤器，若来自菱形非虚继承顶层基类，C++语法访问该名字仍然可能编译二义报错，上层需要额外子对象校验；
     *  2. 被派生类名字隐藏的基类同名成员会被直接排除（派生类同名成员会保留，基类同名候选被整体替换）；
     *  3. 匿名、无标识符成员已经在底层template函数内过滤，本输出不会包含无标识符成员（如匿名位域）。
     *
     * @note 执行原理：
     *  1. 内部调用 collect_nonambiguous_member_infos_template 完成继承链DFS收集、名字隐藏模拟、meta::info去重；
     *  2. 过滤规则：丢弃 vector.size() > 1 的名字项（存在继承名字二义，多基类出现同名成员且派生类没有同名覆盖）；
     *  3. 仅取出每个保留名字对应的唯一 meta::info，输出扁平化成员元信息vector；
     */
    consteval auto collect_nonambiguous_member_infos_coarse_filtered(
        const M::info class_type_info, const M::access_context ctx, const decltype(M::members_of) members_of_func
    ) -> std::vector<M::info> {
        return collect_nonambiguous_member_infos_template(class_type_info, ctx, members_of_func)
            | V::values
            | V::filter([](std::vector<M::info> const& infos) { return infos.size() == 1; })
            | V::transform([](std::vector<M::info> const& infos) { return infos.front(); })
            | R::to<std::vector<M::info>>();
    }

    /**
     * @brief 获取无名字二义的静态数据成员元信息
     * @param class_type_info 目标类的类型反射元信息
     * @param ctx 反射访问上下文，控制访问权限过滤
     * @return std::vector<meta::info> 静态数据成员元信息列表；仅保留名字候选集合size==1、无继承名字二义的静态成员
     * @note 执行链路：
     *  1. 内部直接复用 collect_nonambiguous_member_infos_coarse_filtered，传入 meta::static_data_members_of 获取静态成员；
     *  2. 遵循同样继承名字隐藏规则：派生类静态同名成员会隐藏基类同名静态成员。
     *  3. 静态成员不存在子对象多实例菱形继承问题，不需要额外requires良构校验。
     */
    inline constexpr auto collect_static_member_infos = [](const M::info class_type_info, const M::access_context ctx) -> std::vector<M::info> {
        return collect_nonambiguous_member_infos_coarse_filtered(class_type_info, ctx, M::static_data_members_of);
    };

    /**
     * @brief 获取语法层面可对象直接访问的非静态数据成员元信息
     * @param class_type_info 目标类的类型反射元信息
     * @param ctx 反射访问上下文，控制访问权限过滤
     * @return std::vector<meta::info> 经过两层过滤后的非静态数据成员元信息列表
     * @note 执行链路：
     *  1. 调用 collect_nonambiguous_member_infos_coarse_filtered：
     *     - 内部底层 DFS 遍历继承链，模拟C++派生类名字隐藏规则；
     *     - 粗筛：过滤名字候选集合size>1的**多基类重名继承名字二义**成员；
     *     - 底层链路已过滤无标识符匿名成员（匿名位域等）；
     *  2. 再通过 views::filter 调用 check_syntax_valid 做第二层语法校验：
     *     剔除菱形非虚继承中子对象多实例造成对象`.`访问二义的成员；
     */
    inline constexpr auto collect_nonstatic_member_infos = [](const M::info class_type_info, const M::access_context ctx) consteval -> std::vector<M::info> {
        return collect_nonambiguous_member_infos_coarse_filtered(class_type_info, ctx, M::nonstatic_data_members_of)
        | V::filter(std::bind_front(check_syntax_valid, class_type_info))
        | R::to<std::vector<M::info>>();
    };

    /**
     * @brief 收集经过二义过滤与语法校验的全部数据成员（静态 + 非静态）
     * @param class_type_info 目标类的类型反射元信息
     * @param ctx 反射访问上下文，控制访问权限过滤
     * @return std::vector<meta::info> 合法可访问的数据成员元信息列表
     * @note 执行链路：
     *  1. 调用 collect_nonambiguous_member_infos_coarse_filtered，传入 data_members_of 查询全部数据成员；
     *     - DFS递归遍历完整继承链，模拟C++派生类名字隐藏规则；
     *     - 粗过滤：剔除名字候选集合size>1的多基类重名继承名字二义成员；
     *     - 底层已过滤无标识符匿名成员（匿名位域等）；
     *  2. 流式二次过滤，区分成员类别做语法良构判定：
     *     - 非静态数据成员：调用 check_syntax_valid，校验对象`.`直接访问语法良构性，剔除菱形非虚继承子对象多实例二义的成员；
     *     - 静态数据成员：无实例子对象二义问题，直接放行返回 true；
     */
    inline constexpr auto collect_member_infos = [](const M::info class_type_info, const M::access_context ctx) consteval -> std::vector<M::info> {
        return collect_nonambiguous_member_infos_coarse_filtered(class_type_info, ctx, data_members_of)
        | V::filter([class_type_info](const M::info member_info)
            { return M::is_nonstatic_data_member(member_info) ? check_syntax_valid(class_type_info, member_info) : true; }) // 判定良构仅作用于非静态成员，静态成员直接 true
        | R::to<std::vector<M::info>>();
    };

    /**
     * @brief 最简单实现版本：递归收集当前类型及其全部基类的成员反射元信息
     * @param class_type_info 待解析类型的反射元信息(类/结构体类型)
     * @param ctx 成员访问权限检查上下文
     * @param members_of_func 用于提取某一类型成员的回调函数；接收(meta::info type, meta::access_context ctx)，返回可范围迭代的meta::info序列
     * @return std::vector<meta::info> 合并后的成员元信息数组；顺序：基类成员在前，当前类型自身成员在后
     * @note 采用**后序DFS深度优先遍历继承链**：优先递归遍历所有直接基类，将基类成员全部加入结果；
     *       基类递归全部返回后，再追加当前类型自身的成员；不会做成员去重，若派生类重名覆盖，数组会同时保留基类与派生类同名成员。
     * @attention consteval函数，仅可在编译期执行；返回的vector为编译期编译得到
     */
    inline constexpr auto collect_member_infos_simple_template = [](this auto&& self,
        const M::info class_type_info, const M::access_context ctx, const decltype(M::members_of) members_of_func
    ) consteval -> std::vector<M::info> {
        std::vector<M::info> result;

        // 递归遍历所有直接基类，收集基类的全部成员
        for (const auto base_info : M::bases_of(class_type_info, ctx))
            result.append_range(self(M::type_of(base_info), ctx, members_of_func));

        // 将当前类型自身的成员追加到结果末尾
        result.append_range(members_of_func(class_type_info, ctx));
        return result;
    };
}