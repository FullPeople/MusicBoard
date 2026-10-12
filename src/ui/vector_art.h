#pragma once
#include <array>
#include <cmath>
#include <span>
#include <d2d1.h>
#include <wrl/client.h>
#include "core/failure.h"

namespace musicboard::ui {
[[nodiscard]] inline bool fill_vector(ID2D1RenderTarget& target, ID2D1Brush& brush,
    std::span<const D2D1_POINT_2F> points, Failure& failure) {
    Microsoft::WRL::ComPtr<ID2D1Factory> factory;
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> geometry;
    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
    target.GetFactory(factory.GetAddressOf());
    HRESULT hr = factory ? factory->CreatePathGeometry(geometry.GetAddressOf()) : E_POINTER;
    if (SUCCEEDED(hr)) hr = geometry->Open(sink.GetAddressOf());
    if (FAILED(hr) || points.empty()) {
        failure.set("ui.vector", "Create vector geometry hr=" + std::to_string(hr)); return false;
    }
    sink->BeginFigure(points[0], D2D1_FIGURE_BEGIN_FILLED);
    for (std::size_t i = 1; i < points.size(); ++i) sink->AddLine(points[i]);
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    hr = sink->Close(); sink.Reset();
    if (FAILED(hr)) { failure.set("ui.vector", "Close vector geometry hr=" + std::to_string(hr)); return false; }
    target.FillGeometry(geometry.Get(), &brush); return true;
}
inline void report_vector_failure(const Failure& failure) { ::OutputDebugStringA(failure.to_string().c_str()); }
inline void draw_play_icon(ID2D1RenderTarget& target, ID2D1SolidColorBrush& brush,
    D2D1_POINT_2F center, float size, bool playing) {
    if (playing) {
        for (const float dx : {-0.32f, 0.14f}) target.FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(center.x + dx * size, center.y - size * 0.48f,
                center.x + (dx + 0.2f) * size, center.y + size * 0.48f), 1.0f, 1.0f), &brush);
    } else {
        const std::array<D2D1_POINT_2F,3> points{{{center.x-size*.3f,center.y-size*.48f},
            {center.x+size*.5f,center.y},{center.x-size*.3f,center.y+size*.48f}}};
        Failure failure; if (!fill_vector(target, brush, points, failure)) report_vector_failure(failure);
    }
}
inline void draw_pin_icon(ID2D1RenderTarget& target, ID2D1SolidColorBrush& brush,
    D2D1_POINT_2F center, bool pinned) {
    // Same 24x24 silhouette as design/icons/pin.svg. D2D draws it as vectors.
    constexpr std::array<D2D1_POINT_2F,10> shape{{{7,3},{17,3},{16,6},{16,11},{19,14},
        {13,14},{12,22},{11,14},{5,14},{8,11}}};
    auto points = shape;
    const float a = pinned ? 0.0f : 0.65f;
    for (auto& p : points) {
        const float x=(p.x-12)*.8f, y=(p.y-12)*.8f;
        p={center.x+x*std::cos(a)-y*std::sin(a),center.y+x*std::sin(a)+y*std::cos(a)};
    }
    Failure failure; if (!fill_vector(target, brush, points, failure)) report_vector_failure(failure);
}
inline void draw_vinyl_reflection(ID2D1RenderTarget& target, ID2D1SolidColorBrush& brush,
    D2D1_POINT_2F center, float radius, float angle) {
    for (int opposite=0; opposite<2; ++opposite) for (int layer=0; layer<5; ++layer) {
        const float base=angle-.65f+static_cast<float>(opposite)*3.14159265f;
        const float spread=.23f-static_cast<float>(layer)*.035f;
        std::array<D2D1_POINT_2F,26> points{};
        for (std::size_t i=0;i<13;++i) {
            const float a=base-spread+2*spread*static_cast<float>(i)/12;
            points[i]={center.x+std::cos(a)*radius*.97f,center.y+std::sin(a)*radius*.97f};
            points[25-i]={center.x+std::cos(a)*radius*.37f,center.y+std::sin(a)*radius*.37f};
        }
        brush.SetColor(D2D1::ColorF(1,1,1,opposite ? .012f : .024f));
        Failure failure; if (!fill_vector(target, brush, points, failure)) report_vector_failure(failure);
    }
}
} // namespace musicboard::ui
