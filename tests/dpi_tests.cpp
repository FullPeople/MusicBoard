// DPI 缩放测试：纯函数、完全离线
//
// 这一层的风险不在"能不能算对一次"，而在缩放比一多起来之后有没有哪一档算错。
// 本机是 100%，但用户机器上 125% / 150% / 200% 都很常见，所以每一档都要钉住。
//
// 另外钉住一条运行时不变量：Direct2D render target 的 DPI 必须是 96。
// 不是 96 说明 D2D 参与了缩放，我们自己的乘法缩放会与它叠加，布局整体偏掉。

#include <string>

#include "platform/dpi.h"
#include "tests/test_harness.h"

using namespace musicboard;
using musicboard::test::Registry;

namespace {

void test_identity_at_base_dpi(Registry& r) {
    r.check(scale_for_dpi(100, 96) == 100, "96 DPI 下缩放是恒等变换");
    r.check(scale_for_dpi(kPanelLogicalWidth, 96) == kPanelLogicalWidth, "96 DPI 下面板宽不变");
    r.check(scale_for_dpi(kPanelLogicalHeight, 96) == kPanelLogicalHeight, "96 DPI 下面板高不变");
}

void test_common_scales(Registry& r) {
    // 面板尺寸在常见缩放下的物理像素——这几个数字是布局验收的基准
    struct Case {
        UINT dpi;
        int expected_width;
        int expected_height;
        const char* label;
    };
    const Case cases[] = {
        {120, 1570, 985, "125%"},
        {144, 1884, 1182, "150%"},
        {168, 2198, 1379, "175%"},
        {192, 2512, 1576, "200%"},
    };
    for (const Case& item : cases) {
        r.check_eq(scale_for_dpi(kPanelLogicalWidth, item.dpi) == item.expected_width,
                   std::string("面板宽在 ") + item.label + " 下正确",
                   "dpi=" + std::to_string(item.dpi) + " 期望 " + std::to_string(item.expected_width) +
                       " 实际 " + std::to_string(scale_for_dpi(kPanelLogicalWidth, item.dpi)));
        r.check_eq(scale_for_dpi(kPanelLogicalHeight, item.dpi) == item.expected_height,
                   std::string("面板高在 ") + item.label + " 下正确",
                   "dpi=" + std::to_string(item.dpi) + " 期望 " + std::to_string(item.expected_height) +
                       " 实际 " + std::to_string(scale_for_dpi(kPanelLogicalHeight, item.dpi)));
    }
}

void test_small_metrics(Registry& r) {
    // 小尺寸更容易暴露取整问题：1 逻辑像素在 150% 下是 1.5，MulDiv 取整到 2
    r.check(scale_for_dpi(1, 144) == 2, "1 逻辑像素在 150% 下取整为 2");
    r.check(scale_for_dpi(2, 144) == 3, "2 逻辑像素在 150% 下取整为 3");
    r.check(scale_for_dpi(0, 192) == 0, "0 在任何缩放下都是 0");

    // 标题栏 42 逻辑像素
    r.check(scale_for_dpi(42, 96) == 42, "标题栏 96 DPI 下 42");
    r.check(scale_for_dpi(42, 120) == 53, "标题栏 125% 下 53");
    r.check(scale_for_dpi(42, 144) == 63, "标题栏 150% 下 63");
    r.check(scale_for_dpi(42, 192) == 84, "标题栏 200% 下 84");

    // 圆角 16 逻辑像素
    r.check(scale_for_dpi(16, 144) == 24, "窗口圆角 150% 下 24");
}

void test_round_trip(Registry& r) {
    const UINT dpis[] = {96, 120, 144, 168, 192};
    bool all_ok = true;
    std::string detail;
    for (const UINT dpi : dpis) {
        const int physical = scale_for_dpi(kPanelLogicalWidth, dpi);
        const int back = unscale_for_dpi(physical, dpi);
        if (back != kPanelLogicalWidth) {
            all_ok = false;
            detail += "dpi=" + std::to_string(dpi) + " 往返得到 " + std::to_string(back) + "; ";
        }
    }
    r.check_eq(all_ok, "逻辑→物理→逻辑 在常见 DPI 下都能往返", detail);
}

void test_float_scaling(Registry& r) {
    const float scaled = scale_for_dpi(13.5f, 144);
    r.check(scaled > 20.2f && scaled < 20.3f, "字号 13.5 在 150% 下约 20.25");
    r.check(scale_for_dpi(0.0f, 192) == 0.0f, "0 浮点在缩放下仍是 0");
    // 浮点缩放不做取整：它喂给 DirectWrite，取整会累积成行高误差
    const float odd = scale_for_dpi(10.5f, 120);
    r.check(odd > 13.1f && odd < 13.2f, "字号 10.5 在 125% 下保留小数（13.125）");
}

void test_render_dpi_contract(Registry& r) {
    Failure failure;
    r.check(verify_render_dpi_contract(96.0f, 96.0f, 96, failure), "渲染 DPI 为 96 时通过契约检查");
    r.check(failure.empty(), "通过时不留错误信息");

    // 尾差要容忍：DPI 参与乘除后会有小的浮点误差
    Failure tolerant;
    r.check(verify_render_dpi_contract(96.005f, 95.995f, 96, tolerant), "96 附近的尾差被容忍");

    // 真正的违约：D2D 按显示器 DPI 自己缩了一次
    Failure violated;
    r.check(!verify_render_dpi_contract(144.0f, 144.0f, 144, violated),
            "渲染 DPI 不等于 96 时契约失败");
    r.check(!violated.code.empty(), "契约失败带错误码");
    r.check(violated.context.find("96") != std::string::npos,
            "契约失败的上下文里点明了期望值 96");
    r.check(violated.context.find("144") != std::string::npos,
            "契约失败的上下文里给出了实际值");
}

}  // namespace

int main() {
    Registry r;
    test_identity_at_base_dpi(r);
    test_common_scales(r);
    test_small_metrics(r);
    test_round_trip(r);
    test_float_scaling(r);
    test_render_dpi_contract(r);
    Failure enable_failure;
    r.check_invocation([&] { return enable_per_monitor_dpi(enable_failure); }, "enable PerMonitorV2", enable_failure);
    const auto previous = ::SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE);
    r.check(previous != nullptr, "prepare incompatible thread DPI context");
    if (previous) {
        Failure incompatible;
        const bool accepted = enable_per_monitor_dpi(incompatible);
        r.check(!accepted && !incompatible.code.empty() && !incompatible.context.empty(),
                "already configured but incompatible awareness must fail explicitly");
        r.check(::SetThreadDpiAwarenessContext(previous) != nullptr, "restore original thread DPI context");
    }
    return r.finish("dpi_tests");
}
