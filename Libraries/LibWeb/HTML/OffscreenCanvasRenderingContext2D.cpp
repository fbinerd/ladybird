/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/OwnPtr.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibGfx/CompositingAndBlendingOperator.h>
#include <LibGfx/FontCascadeList.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibGfx/PainterSkia.h>
#include <LibGfx/Rect.h>
#include <LibGfx/TextLayout.h>
#include <LibUnicode/Segmenter.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/OffscreenCanvasRenderingContext2D.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/HTML/CanvasRenderingContext2D.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/ImageBitmap.h>
#include <LibWeb/HTML/ImageData.h>
#include <LibWeb/HTML/OffscreenCanvas.h>
#include <LibWeb/HTML/OffscreenCanvasRenderingContext2D.h>
#include <LibWeb/HTML/Path2D.h>
#include <LibWeb/HTML/TextMetrics.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/Infra/CharacterTypes.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/Platform/FontPlugin.h>
#include <LibWeb/SVG/SVGImageElement.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(OffscreenCanvasRenderingContext2D);

JS::ThrowCompletionOr<GC::Ref<OffscreenCanvasRenderingContext2D>> OffscreenCanvasRenderingContext2D::create(JS::Realm& realm, OffscreenCanvas& offscreen_canvas, JS::Value options)
{
    auto context_attributes = TRY(CanvasRenderingContext2DSettings::from_js_value(realm.vm(), options));
    return realm.create<OffscreenCanvasRenderingContext2D>(realm, offscreen_canvas, context_attributes);
}

OffscreenCanvasRenderingContext2D::OffscreenCanvasRenderingContext2D(JS::Realm& realm, OffscreenCanvas& offscreen_canvas, CanvasRenderingContext2DSettings context_attributes)
    : PlatformObject(realm)
    , CanvasPath(static_cast<Bindings::PlatformObject&>(*this), *this)
    , m_canvas(offscreen_canvas)
    , m_size(offscreen_canvas.bitmap_size_for_canvas())
    , m_context_attributes(context_attributes)
{
}

OffscreenCanvasRenderingContext2D::~OffscreenCanvasRenderingContext2D() = default;

void OffscreenCanvasRenderingContext2D::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    set_prototype(&Bindings::ensure_web_prototype<Bindings::OffscreenCanvasRenderingContext2DPrototype>(realm, "OffscreenCanvasRenderingContext2D"_string));
}

void OffscreenCanvasRenderingContext2D::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    CanvasState::visit_edges(visitor);
    visitor.visit(m_canvas);
}

void OffscreenCanvasRenderingContext2D::set_size(Gfx::IntSize const& size)
{
    if (m_size == size)
        return;
    m_size = size;
    m_painter = nullptr;
}

GC::Ref<OffscreenCanvas> OffscreenCanvasRenderingContext2D::canvas()
{
    return m_canvas;
}

OffscreenCanvas& OffscreenCanvasRenderingContext2D::canvas_element()
{
    return *m_canvas;
}

OffscreenCanvas const& OffscreenCanvasRenderingContext2D::canvas_element() const
{

    return *m_canvas;
}

Gfx::Path OffscreenCanvasRenderingContext2D::rect_path(float x, float y, float width, float height)
{
    auto top_left = Gfx::FloatPoint(x, y);
    auto top_right = Gfx::FloatPoint(x + width, y);
    auto bottom_left = Gfx::FloatPoint(x, y + height);
    auto bottom_right = Gfx::FloatPoint(x + width, y + height);

    Gfx::Path path;
    path.move_to(top_left);
    path.line_to(top_right);
    path.line_to(bottom_right);
    path.line_to(bottom_left);
    path.line_to(top_left);
    return path;
}

static Gfx::WindingRule parse_fill_rule(StringView fill_rule)
{
    if (fill_rule == "evenodd"sv)
        return Gfx::WindingRule::EvenOdd;
    if (fill_rule == "nonzero"sv)
        return Gfx::WindingRule::Nonzero;
    dbgln("Unrecognized fillRule for OffscreenCanvasRenderingContext2D.fill()");
    return Gfx::WindingRule::Nonzero;
}

static Gfx::Path::CapStyle to_gfx_cap(Bindings::CanvasLineCap const& cap_style)
{
    switch (cap_style) {
    case Bindings::CanvasLineCap::Butt:
        return Gfx::Path::CapStyle::Butt;
    case Bindings::CanvasLineCap::Round:
        return Gfx::Path::CapStyle::Round;
    case Bindings::CanvasLineCap::Square:
        return Gfx::Path::CapStyle::Square;
    }
    VERIFY_NOT_REACHED();
}

static Gfx::Path::JoinStyle to_gfx_join(Bindings::CanvasLineJoin const& join_style)
{
    switch (join_style) {
    case Bindings::CanvasLineJoin::Round:
        return Gfx::Path::JoinStyle::Round;
    case Bindings::CanvasLineJoin::Bevel:
        return Gfx::Path::JoinStyle::Bevel;
    case Bindings::CanvasLineJoin::Miter:
        return Gfx::Path::JoinStyle::Miter;
    }
    VERIFY_NOT_REACHED();
}

Gfx::Color OffscreenCanvasRenderingContext2D::clear_color() const
{
    return m_context_attributes.alpha ? Gfx::Color::Transparent : Gfx::Color::Black;
}

void OffscreenCanvasRenderingContext2D::did_draw(Gfx::FloatRect const&)
{
    // OffscreenCanvas has no element invalidation hook; consumers read its backing bitmap directly.
}

void OffscreenCanvasRenderingContext2D::fill_internal(Gfx::Path const& path, Gfx::WindingRule winding_rule)
{
    auto* painter = this->painter();
    if (!painter)
        return;

    auto& state = drawing_state();
    auto paint_style = state.fill_style.to_gfx_paint_style();
    if (!paint_style->is_visible())
        return;

    painter->fill_path(path, paint_style, state.filter, state.global_alpha, state.current_compositing_and_blending_operator, winding_rule);
    did_draw(path.bounding_box());
}

void OffscreenCanvasRenderingContext2D::stroke_internal(Gfx::Path const& path)
{
    auto* painter = this->painter();
    if (!painter)
        return;

    auto& state = drawing_state();
    auto paint_style = state.stroke_style.to_gfx_paint_style();
    if (!paint_style->is_visible())
        return;

    auto dash_array = Vector<float> {};
    dash_array.ensure_capacity(state.dash_list.size());
    for (auto const& dash : state.dash_list)
        dash_array.append(static_cast<float>(dash));

    painter->stroke_path(path, paint_style, state.filter, state.line_width, state.global_alpha, state.current_compositing_and_blending_operator, to_gfx_cap(state.line_cap), to_gfx_join(state.line_join), state.miter_limit, dash_array, state.line_dash_offset);
    did_draw(path.bounding_box());
}

void OffscreenCanvasRenderingContext2D::fill_rect(float x, float y, float width, float height)
{
    fill_internal(rect_path(x, y, width, height), Gfx::WindingRule::EvenOdd);
}

void OffscreenCanvasRenderingContext2D::clear_rect(float x, float y, float width, float height)
{
    if (!isfinite(x) || !isfinite(y) || !isfinite(width) || !isfinite(height))
        return;

    if (auto* painter = this->painter()) {
        auto rect = Gfx::FloatRect(x, y, width, height);
        painter->clear_rect(rect, clear_color());
        did_draw(rect);
    }
}

void OffscreenCanvasRenderingContext2D::stroke_rect(float x, float y, float width, float height)
{
    stroke_internal(rect_path(x, y, width, height));
}

WebIDL::ExceptionOr<void> OffscreenCanvasRenderingContext2D::draw_image_internal(CanvasImageSource const& image, float source_x, float source_y, float source_width, float source_height, float destination_x, float destination_y, float destination_width, float destination_height)
{
    if (!isfinite(source_x) || !isfinite(source_y) || !isfinite(source_width) || !isfinite(source_height) || !isfinite(destination_x) || !isfinite(destination_y) || !isfinite(destination_width) || !isfinite(destination_height))
        return {};

    auto usability = TRY(check_usability_of_image(image));
    if (usability == CanvasImageSourceUsability::Bad)
        return {};

    if (source_width < 0) {
        source_x += source_width;
        source_width = abs(source_width);
    }
    if (source_height < 0) {
        source_y += source_height;
        source_height = abs(source_height);
    }
    if (destination_width < 0) {
        destination_x += destination_width;
        destination_width = abs(destination_width);
    }
    if (destination_height < 0) {
        destination_y += destination_height;
        destination_height = abs(destination_height);
    }

    auto image_size = canvas_image_source_dimensions(image);
    if (image_size.is_empty())
        return {};

    if (source_width == 0 || source_height == 0)
        return {};

    auto source_rect = Gfx::FloatRect { source_x, source_y, source_width, source_height };
    auto destination_rect = Gfx::FloatRect { destination_x, destination_y, destination_width, destination_height };
    auto clipped_source = source_rect.intersected(Gfx::IntRect { {}, image_size }.to_type<float>());
    auto clipped_destination = destination_rect;
    if (clipped_source != source_rect) {
        clipped_destination.set_width(clipped_destination.width() * (clipped_source.width() / source_rect.width()));
        clipped_destination.set_height(clipped_destination.height() * (clipped_source.height() / source_rect.height()));
    }

    auto scaling_mode = drawing_state().image_smoothing_enabled ? Gfx::ScalingMode::BilinearMipmap : Gfx::ScalingMode::NearestNeighbor;
    if (auto* painter = this->painter()) {
        if (auto const* frame = canvas_image_source_video_frame(image); frame && frame->nv12_data()) {
            auto target_size = clipped_destination.to_rounded<int>().size();
            if (!target_size.is_empty()) {
                auto scaled_bitmap = create_scaled_bitmap_from_video_frame(*frame, clipped_source.to_rounded<int>(), target_size);
                if (!scaled_bitmap.is_error()) {
                    static size_t s_nv12_offscreen_canvas_draw_count = 0;
                    s_nv12_offscreen_canvas_draw_count++;
                    if (s_nv12_offscreen_canvas_draw_count <= 8 || s_nv12_offscreen_canvas_draw_count % 120 == 0)
                        dbgln("MUNDO_OFFSCREEN_CANVAS_VIDEO_NV12_DRAW count={} target={}x{} source={}x{} destination={}x{}", s_nv12_offscreen_canvas_draw_count, target_size.width(), target_size.height(), clipped_source.width(), clipped_source.height(), clipped_destination.width(), clipped_destination.height());
                    auto immutable = Gfx::ImmutableBitmap::create(scaled_bitmap.release_value());
                    painter->draw_bitmap(clipped_destination, *immutable, immutable->rect(), Gfx::ScalingMode::NearestNeighbor, drawing_state().filter, drawing_state().global_alpha, drawing_state().current_compositing_and_blending_operator);
                    did_draw(clipped_destination);
                    return {};
                }
                dbgln("MUNDO_OFFSCREEN_CANVAS_VIDEO_NV12_DRAW failed target={}x{} error={}", target_size.width(), target_size.height(), scaled_bitmap.error());
            }
        }

        auto bitmap = canvas_image_source_bitmap(image);
        if (!bitmap)
            return {};
        painter->draw_bitmap(destination_rect, *bitmap, source_rect.to_rounded<int>(), scaling_mode, drawing_state().filter, drawing_state().global_alpha, drawing_state().current_compositing_and_blending_operator);
        did_draw(destination_rect);
    }

    return {};
}

void OffscreenCanvasRenderingContext2D::begin_path()
{
    path().clear();
}

void OffscreenCanvasRenderingContext2D::stroke()
{
    stroke_internal(path());
}

void OffscreenCanvasRenderingContext2D::stroke(Path2D const& path)
{
    stroke_internal(path.path());
}

void OffscreenCanvasRenderingContext2D::fill_text(Utf16String const& text, float x, float y, Optional<double> max_width)
{
    if (!isfinite(x) || !isfinite(y) || (max_width.has_value() && !isfinite(max_width.value())))
        return;

    fill_internal(text_path(text, x, y, max_width), Gfx::WindingRule::Nonzero);
}

void OffscreenCanvasRenderingContext2D::stroke_text(Utf16String const& text, float x, float y, Optional<double> max_width)
{
    if (!isfinite(x) || !isfinite(y) || (max_width.has_value() && !isfinite(max_width.value())))
        return;

    stroke_internal(text_path(text, x, y, max_width));
}

void OffscreenCanvasRenderingContext2D::fill(StringView fill_rule)
{
    fill_internal(path(), parse_fill_rule(fill_rule));
}

void OffscreenCanvasRenderingContext2D::fill(Path2D& path, StringView fill_rule)
{
    fill_internal(path.path(), parse_fill_rule(fill_rule));
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-createimagedata
WebIDL::ExceptionOr<GC::Ref<ImageData>> OffscreenCanvasRenderingContext2D::create_image_data(int width, int height, Optional<ImageDataSettings> const& settings) const
{
    if (width == 0 || height == 0)
        return WebIDL::IndexSizeError::create(realm(), "Width and height must not be zero"_utf16);

    return TRY(ImageData::create(realm(), abs(width), abs(height), settings));
}

WebIDL::ExceptionOr<GC::Ref<ImageData>> OffscreenCanvasRenderingContext2D::create_image_data(ImageData const& image_data) const
{
    return TRY(ImageData::create(realm(), image_data.width(), image_data.height()));
}

WebIDL::ExceptionOr<GC::Ptr<ImageData>> OffscreenCanvasRenderingContext2D::get_image_data(int x, int y, int width, int height, Optional<ImageDataSettings> const& settings) const
{
    if (width == 0 || height == 0)
        return WebIDL::IndexSizeError::create(realm(), "Width and height must not be zero"_utf16);

    int abs_width = abs(width);
    int abs_height = abs(height);
    auto image_data = TRY(ImageData::create(realm(), abs_width, abs_height, settings));

    auto bitmap = canvas_element().bitmap();
    if (!bitmap)
        return image_data;

    auto source_rect = Gfx::Rect { x, y, abs_width, abs_height };
    if (width < 0 || height < 0)
        source_rect = source_rect.translated(min(width, 0), min(height, 0));

    auto source_bitmap = Gfx::ImmutableBitmap::create(*bitmap, Gfx::AlphaType::Premultiplied);
    auto source_rect_intersected = source_rect.intersected(source_bitmap->rect());

    auto painter = Gfx::Painter::create(image_data->bitmap());
    painter->draw_bitmap(image_data->bitmap().rect().to_type<float>(), source_bitmap, source_rect_intersected, Gfx::ScalingMode::NearestNeighbor, {}, 1, Gfx::CompositingAndBlendingOperator::SourceOver);

    return image_data;
}

WebIDL::ExceptionOr<void> OffscreenCanvasRenderingContext2D::put_image_data(ImageData& image_data, float dx, float dy)
{
    if (auto* buffer = image_data.data()->viewed_array_buffer(); buffer->is_detached())
        return WebIDL::InvalidStateError::create(image_data.realm(), "ImageData's underlying buffer is detached"_utf16);

    if (auto* painter = this->painter()) {
        painter->save();
        painter->set_transform({});
        painter->draw_bitmap(
            { dx, dy, static_cast<float>(image_data.width()), static_cast<float>(image_data.height()) },
            Gfx::ImmutableBitmap::create(image_data.bitmap(), Gfx::AlphaType::Unpremultiplied),
            image_data.bitmap().rect(),
            Gfx::ScalingMode::NearestNeighbor,
            {},
            1.0f,
            Gfx::CompositingAndBlendingOperator::SourceOver);
        painter->restore();
        did_draw({ dx, dy, static_cast<float>(image_data.width()), static_cast<float>(image_data.height()) });
    }

    return {};
}

WebIDL::ExceptionOr<void> OffscreenCanvasRenderingContext2D::put_image_data(ImageData& image_data, float dx, float dy, float dirty_x, float dirty_y, float dirty_width, float dirty_height)
{
    if (auto* buffer = image_data.data()->viewed_array_buffer(); buffer->is_detached())
        return WebIDL::InvalidStateError::create(image_data.realm(), "ImageData's underlying buffer is detached"_utf16);

    if (dirty_width < 0) {
        dirty_x += dirty_width;
        dirty_width = abs(dirty_width);
    }
    if (dirty_height < 0) {
        dirty_y += dirty_height;
        dirty_height = abs(dirty_height);
    }
    if (dirty_x < 0) {
        dirty_width += dirty_x;
        dirty_x = 0;
    }
    if (dirty_y < 0) {
        dirty_height += dirty_y;
        dirty_y = 0;
    }
    if (dirty_x + dirty_width > image_data.width())
        dirty_width = image_data.width() - dirty_x;
    if (dirty_y + dirty_height > image_data.height())
        dirty_height = image_data.height() - dirty_y;
    if (dirty_width <= 0 || dirty_height <= 0)
        return {};

    if (auto* painter = this->painter()) {
        auto dst_rect = Gfx::FloatRect { dx + dirty_x, dy + dirty_y, dirty_width, dirty_height };
        painter->save();
        painter->set_transform({});
        painter->draw_bitmap(
            dst_rect,
            Gfx::ImmutableBitmap::create(image_data.bitmap(), Gfx::AlphaType::Unpremultiplied),
            Gfx::IntRect { static_cast<int>(dirty_x), static_cast<int>(dirty_y), static_cast<int>(dirty_width), static_cast<int>(dirty_height) },
            Gfx::ScalingMode::NearestNeighbor,
            {},
            1.0f,
            Gfx::CompositingAndBlendingOperator::SourceOver);
        painter->restore();
        did_draw(dst_rect);
    }

    return {};
}

void OffscreenCanvasRenderingContext2D::reset_to_default_state()
{
    if (auto* painter = this->painter()) {
        painter->clear_rect({ 0, 0, static_cast<float>(m_size.width()), static_cast<float>(m_size.height()) }, clear_color());
        painter->reset();
    }

    path().clear();
    clear_drawing_state_stack();
    reset_drawing_state();
}

GC::Ref<TextMetrics> OffscreenCanvasRenderingContext2D::measure_text(Utf16String const& text)
{
    auto prepared_text = prepare_text(text);
    auto metrics = TextMetrics::create(realm());
    auto const& font = font_cascade_list()->first();
    auto const& font_pixel_metrics = font.pixel_metrics();
    auto const ascent = font_pixel_metrics.ascent;
    auto const descent = font_pixel_metrics.descent;
    auto const hanging_baseline = ascent * 0.8f;

    float baseline_offset = 0;
    switch (drawing_state().text_baseline) {
    case Bindings::CanvasTextBaseline::Top:
        baseline_offset = ascent;
        break;
    case Bindings::CanvasTextBaseline::Hanging:
        baseline_offset = hanging_baseline;
        break;
    case Bindings::CanvasTextBaseline::Middle:
        baseline_offset = (ascent - descent) / 2.0f;
        break;
    case Bindings::CanvasTextBaseline::Alphabetic:
        baseline_offset = 0;
        break;
    case Bindings::CanvasTextBaseline::Ideographic:
    case Bindings::CanvasTextBaseline::Bottom:
        baseline_offset = -descent;
        break;
    }

    metrics->set_width(prepared_text.bounding_box.width());
    metrics->set_actual_bounding_box_left(-prepared_text.bounding_box.left());
    metrics->set_actual_bounding_box_right(prepared_text.bounding_box.right());
    metrics->set_font_bounding_box_ascent(ascent - baseline_offset);
    metrics->set_font_bounding_box_descent(descent + baseline_offset);
    metrics->set_actual_bounding_box_ascent(ascent - baseline_offset);
    metrics->set_actual_bounding_box_descent(descent + baseline_offset);
    metrics->set_em_height_ascent(ascent - baseline_offset);
    metrics->set_em_height_descent(descent + baseline_offset);
    metrics->set_hanging_baseline(hanging_baseline - baseline_offset);
    metrics->set_alphabetic_baseline(-baseline_offset);
    metrics->set_ideographic_baseline(-descent - baseline_offset);

    return metrics;
}

RefPtr<Gfx::FontCascadeList const> OffscreenCanvasRenderingContext2D::font_cascade_list()
{
    if (!drawing_state().font_style_value)
        set_font("10px sans-serif"sv);

    if (!drawing_state().current_font_cascade_list) {
        auto font_list = Gfx::FontCascadeList::create();
        if (auto default_font = Platform::FontPlugin::the().default_font(8)) {
            font_list->add(*default_font);
            font_list->set_last_resort_font(*default_font);
        }
        drawing_state().current_font_cascade_list = font_list;
    }

    return drawing_state().current_font_cascade_list;
}

static float resolved_offscreen_letter_spacing(CanvasState::DrawingState const& drawing_state, OffscreenCanvas const& canvas)
{
    auto computation_context = canvas.canvas_font_computation_context();
    return static_cast<float>(drawing_state.letter_spacing.to_px(computation_context.length_resolution_context).to_double());
}

OffscreenCanvasRenderingContext2D::PreparedText OffscreenCanvasRenderingContext2D::prepare_text(Utf16String const& text, float max_width)
{
    if (max_width <= 0 || max_width != max_width)
        return {};

    StringBuilder builder { StringBuilder::Mode::UTF16, text.length_in_code_units() };
    for (auto c : text)
        builder.append_code_point(Infra::is_ascii_whitespace(c) ? ' ' : c);
    auto replaced_text = builder.to_utf16_string();

    auto glyph_runs = Gfx::shape_text({ 0, 0 }, replaced_text.utf16_view(), *font_cascade_list(),
        resolved_offscreen_letter_spacing(drawing_state(), canvas_element()));

    float height = 0;
    float width = 0;
    for (auto const& glyph_run : glyph_runs) {
        height = max(height, glyph_run->font().pixel_size());
        width += glyph_run->width();
    }

    return { move(glyph_runs), Gfx::TextAlignment::CenterLeft, { 0, 0, width, height } };
}

Gfx::Path OffscreenCanvasRenderingContext2D::text_path(Utf16String const& text, float x, float y, Optional<double> max_width)
{
    if (max_width.has_value() && max_width.value() <= 0)
        return {};

    auto& drawing_state = this->drawing_state();
    auto const& font_cascade_list = this->font_cascade_list();
    auto const& font = font_cascade_list->first();
    auto glyph_runs = Gfx::shape_text({ x, y }, text.utf16_view(), *font_cascade_list,
        resolved_offscreen_letter_spacing(drawing_state, canvas_element()));

    Gfx::Path path;
    for (auto const& glyph_run : glyph_runs)
        path.glyph_run(glyph_run);

    auto text_width = path.bounding_box().width();
    Gfx::AffineTransform transform = {};
    if (max_width.has_value() && text_width > float(*max_width)) {
        auto horizontal_scale = float(*max_width) / text_width;
        transform = Gfx::AffineTransform {}.scale({ horizontal_scale, 1 });
        text_width *= horizontal_scale;
    }

    bool is_rtl = drawing_state.direction == Bindings::CanvasDirection::Rtl;
    if (drawing_state.text_align == Bindings::CanvasTextAlign::Center) {
        transform = Gfx::AffineTransform {}.set_translation({ -text_width / 2, 0 }).multiply(transform);
    } else if (drawing_state.text_align == Bindings::CanvasTextAlign::Start) {
        if (is_rtl)
            transform = Gfx::AffineTransform {}.set_translation({ -text_width, 0 }).multiply(transform);
    } else if (drawing_state.text_align == Bindings::CanvasTextAlign::End) {
        if (!is_rtl)
            transform = Gfx::AffineTransform {}.set_translation({ -text_width, 0 }).multiply(transform);
    } else if (drawing_state.text_align == Bindings::CanvasTextAlign::Right) {
        transform = Gfx::AffineTransform {}.set_translation({ -text_width, 0 }).multiply(transform);
    }

    auto const& font_pixel_metrics = font.pixel_metrics();
    auto baseline_y_offset = [&] {
        switch (drawing_state.text_baseline) {
        case Bindings::CanvasTextBaseline::Top:
            return font_pixel_metrics.ascent;
        case Bindings::CanvasTextBaseline::Hanging:
            return font_pixel_metrics.ascent * 0.8f;
        case Bindings::CanvasTextBaseline::Middle:
            return (font_pixel_metrics.ascent - font_pixel_metrics.descent) / 2.0f;
        case Bindings::CanvasTextBaseline::Alphabetic:
            return 0.0f;
        case Bindings::CanvasTextBaseline::Ideographic:
        case Bindings::CanvasTextBaseline::Bottom:
            return -font_pixel_metrics.descent;
        }
        VERIFY_NOT_REACHED();
    }();

    if (baseline_y_offset != 0.f)
        transform = Gfx::AffineTransform {}.set_translation({ 0, baseline_y_offset }).multiply(transform);

    return path.copy_transformed(transform);
}

void OffscreenCanvasRenderingContext2D::clip(StringView)
{
    dbgln("(STUBBED) OffscreenCanvasRenderingContext2D::clip(StringView)");
}

void OffscreenCanvasRenderingContext2D::clip(Path2D&, StringView)
{
    dbgln("(STUBBED) OffscreenCanvasRenderingContext2D::clip(Path2D&, StringView)");
}

bool OffscreenCanvasRenderingContext2D::is_point_in_path(double, double, StringView)
{
    dbgln("(STUBBED) OffscreenCanvasRenderingContext2D::is_point_in_path(double, double, StringView)");
    return false;
}

bool OffscreenCanvasRenderingContext2D::is_point_in_path(Path2D const&, double, double, StringView)
{
    dbgln("(STUBBED) OffscreenCanvasRenderingContext2D::clip(Path2D const&, double, double, StringView)");
    return false;
}

bool OffscreenCanvasRenderingContext2D::image_smoothing_enabled() const
{
    return drawing_state().image_smoothing_enabled;
}

void OffscreenCanvasRenderingContext2D::set_image_smoothing_enabled(bool enabled)
{
    drawing_state().image_smoothing_enabled = enabled;
}

Bindings::ImageSmoothingQuality OffscreenCanvasRenderingContext2D::image_smoothing_quality() const
{
    return drawing_state().image_smoothing_quality;
}

void OffscreenCanvasRenderingContext2D::set_image_smoothing_quality(Bindings::ImageSmoothingQuality quality)
{
    drawing_state().image_smoothing_quality = quality;
}

String OffscreenCanvasRenderingContext2D::filter() const
{
    dbgln("(STUBBED) OffscreenCanvasRenderingContext2D::filter()");
    return String::from_utf8_without_validation("none"sv.bytes());
}

void OffscreenCanvasRenderingContext2D::set_filter(String)
{
    dbgln("(STUBBED) OffscreenCanvasRenderingContext2D::set_filter()");
}

float OffscreenCanvasRenderingContext2D::shadow_offset_x() const
{
    return drawing_state().shadow_offset_x;
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-shadowoffsetx
void OffscreenCanvasRenderingContext2D::set_shadow_offset_x(float offset_x)
{
    // On setting, the attribute being set must be set to the new value, except if the value is infinite or NaN,
    // in which case the new value must be ignored.
    if (isinf(offset_x) || isnan(offset_x))
        return;

    drawing_state().shadow_offset_x = offset_x;
}

float OffscreenCanvasRenderingContext2D::shadow_offset_y() const
{
    return drawing_state().shadow_offset_y;
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-shadowoffsety
void OffscreenCanvasRenderingContext2D::set_shadow_offset_y(float offset_y)
{
    // On setting, the attribute being set must be set to the new value, except if the value is infinite or NaN,
    // in which case the new value must be ignored.
    if (isinf(offset_y) || isnan(offset_y))
        return;

    drawing_state().shadow_offset_y = offset_y;
}

float OffscreenCanvasRenderingContext2D::shadow_blur() const
{
    return drawing_state().shadow_blur;
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-shadowblur
void OffscreenCanvasRenderingContext2D::set_shadow_blur(float blur_radius)
{
    // On setting, the attribute must be set to the new value,
    // except if the value is negative, infinite or NaN, in which case the new value must be ignored.
    if (blur_radius < 0 || isinf(blur_radius) || isnan(blur_radius))
        return;

    drawing_state().shadow_blur = blur_radius;
}

String OffscreenCanvasRenderingContext2D::shadow_color() const
{
    // https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-shadowcolor
    return drawing_state().shadow_color.to_string(Gfx::Color::HTMLCompatibleSerialization::Yes);
}

void OffscreenCanvasRenderingContext2D::set_shadow_color(String color)
{
    // 1. Let context be this's canvas attribute's value, if that is an element; otherwise null.

    // 2. Let parsedValue be the result of parsing the given value with context if non-null.
    auto style_value = parse_css_value(CSS::Parser::ParsingParams { CSS::Parser::SpecialContext::CanvasContextGenericValue }, color, CSS::PropertyID::Color);
    if (style_value && style_value->has_color()) {
        auto parsedValue = style_value->to_color({}).value_or(Color::Black);

        // 4. Set this's shadow color to parsedValue.
        drawing_state().shadow_color = parsedValue;
    } else {
        // 3. If parsedValue is failure, then return.
        return;
    }
}

float OffscreenCanvasRenderingContext2D::global_alpha() const
{
    return drawing_state().global_alpha;
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-globalalpha
void OffscreenCanvasRenderingContext2D::set_global_alpha(float alpha)
{
    // 1. If the given value is either infinite, NaN, or not in the range 0.0 to 1.0, then return.
    if (!isfinite(alpha) || alpha < 0.0f || alpha > 1.0f) {
        return;
    }
    // 2. Otherwise, set this's global alpha to the given value.
    drawing_state().global_alpha = alpha;
}

String OffscreenCanvasRenderingContext2D::global_composite_operation() const
{
    dbgln("(STUBBED) OffscreenCanvasRenderingContext2D::global_composite_operation()");
    return String::from_utf8_without_validation(""sv.bytes());
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-globalcompositeoperation
void OffscreenCanvasRenderingContext2D::set_global_composite_operation(String)
{
    dbgln("(STUBBED) OffscreenCanvasRenderingContext2D::set_global_composite_operation()");
}

[[nodiscard]] Gfx::Painter* OffscreenCanvasRenderingContext2D::painter()
{
    auto bitmap = canvas_element().bitmap();
    if (!bitmap)
        return nullptr;

    if (!m_painter)
        m_painter = Gfx::Painter::create(*bitmap);
    return m_painter.ptr();
}

}
