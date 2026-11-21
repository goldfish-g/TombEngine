#include "framework.h"
#include "Renderer/Renderer.h"

#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include <nanosvg/nanosvg.h>
#include <nanosvg/nanosvgrast.h>

#include "Renderer/TouchIcons.h"
#include "Specific/Input/TouchControls.h"

using namespace TEN::Input;

// The on-screen touch controls are SVGs (see Tools/TouchIcons), drawn with nanosvg at the
// size they show on screen, so they stay sharp at any size. Each drawing is kept until it
// stops showing at that size.

namespace TEN::Renderer
{
	constexpr auto TOUCH_ICON_COUNT				 = (int)TouchIcon::Count;
	constexpr auto TOUCH_IMAGE_UNUSED_FRAMES_MAX = 120;
	constexpr auto TOUCH_RING_WIDTH_STEPS		 = 4.0f; // Per pixel, for drawings kept by ring width.

	static Vertex GetTouchVertex(const Vector2& point, const Vector2& screenSize, const Vector2& uv, const Vector4& color)
	{
		auto vertex = Vertex{};
		vertex.Position = Vector3(((point.x / screenSize.x) * 2.0f) - 1.0f, 1.0f - ((point.y / screenSize.y) * 2.0f), 0.0f);
		vertex.UV = uv;
		vertex.Color = VectorColorToRGBA(color);
		return vertex;
	}

	static void DrawTouchQuad(IPrimitiveBatch& batch, const Vector2& topLeft, const Vector2& bottomRight, const Vector2& screenSize,
							  const Vector2& uvStart, const Vector2& uvEnd, const Vector4& color)
	{
		batch.DrawQuad(
			GetTouchVertex(topLeft, screenSize, uvStart, color),
			GetTouchVertex(Vector2(bottomRight.x, topLeft.y), screenSize, Vector2(uvEnd.x, uvStart.y), color),
			GetTouchVertex(bottomRight, screenSize, uvEnd, color),
			GetTouchVertex(Vector2(topLeft.x, bottomRight.y), screenSize, Vector2(uvStart.x, uvEnd.y), color));
	}

	// Gives a "ring" shape another stroke width, keeping its outer edge where the SVG has it.
	static void FitTouchRing(NSVGshape& shape, float width)
	{
		float radius = (shape.bounds[2] - shape.bounds[0]) / 2.0f;
		if (radius <= 0.0f)
			return;

		auto center = Vector2((shape.bounds[0] + shape.bounds[2]) / 2.0f, (shape.bounds[1] + shape.bounds[3]) / 2.0f);
		float outerRadius = radius + (shape.strokeWidth / 2.0f);
		float scale = std::max(outerRadius - (width / 2.0f), 0.0f) / radius;

		for (auto* path = shape.paths; path != nullptr; path = path->next)
		{
			for (int i = 0; i < path->npts; i++)
			{
				path->pts[i * 2] = center.x + ((path->pts[i * 2] - center.x) * scale);
				path->pts[(i * 2) + 1] = center.y + ((path->pts[(i * 2) + 1] - center.y) * scale);
			}
		}

		shape.strokeWidth = width;
	}

	// An SVG drawn into the RGBA pixels of a square of the given side.
	static std::vector<unsigned char> RasterizeTouchSvg(const char* svg, int size, float ringWidth)
	{
		static auto* rasterizer = nsvgCreateRasterizer();

		// nanosvg writes into the text it parses.
		auto text = std::string(svg);
		auto* image = nsvgParse(text.data(), "px", 96.0f);
		if (image == nullptr)
			return {};

		auto pixels = std::vector<unsigned char>();
		if (image->width > 0.0f && rasterizer != nullptr)
		{
			float scale = size / image->width;
			if (ringWidth > 0.0f)
			{
				for (auto* shape = image->shapes; shape != nullptr; shape = shape->next)
				{
					if (strcmp(shape->id, "ring") == 0)
						FitTouchRing(*shape, ringWidth / scale);
				}
			}

			pixels.resize(size * size * 4);
			nsvgRasterize(rasterizer, image, 0.0f, 0.0f, scale, pixels.data(), size, size, size * 4);
		}

		nsvgDelete(image);
		return pixels;
	}

	// The drawing of an icon (TouchIcon) or, after the icons, a circle (TouchShape).
	ITexture2D* Renderer::GetTouchImage(int imageID, int size, float ringWidth)
	{
		int ringSteps = (int)std::round(ringWidth * TOUCH_RING_WIDTH_STEPS);
		auto key = ((unsigned long long)imageID << 40) | ((unsigned long long)size << 16) | (unsigned long long)ringSteps;

		auto it = _touchImages.find(key);
		if (it == _touchImages.end())
		{
			const char* svg = (imageID < TOUCH_ICON_COUNT) ? TOUCH_ICON_SVGS[imageID] : TOUCH_SHAPE_SVGS[imageID - TOUCH_ICON_COUNT];
			auto pixels = RasterizeTouchSvg(svg, size, ringSteps / TOUCH_RING_WIDTH_STEPS);
			if (pixels.empty())
			{
				TENLog("Failed to draw image " + std::to_string(imageID) + " of the touch controls.", LogLevel::Warning);
				return nullptr;
			}

			auto texture = _graphicsDevice->CreateTexture2D(size, size, SurfaceFormat::SF_RGBA8_Unorm, pixels.data());
			it = _touchImages.emplace(key, std::make_pair(std::move(texture), _touchFrame)).first;
		}

		it->second.second = _touchFrame;
		return it->second.first.get();
	}

	// A drawing at its own size, on whole pixels so it shows exactly as drawn.
	void Renderer::DrawTouchImage(int imageID, const Vector2& center, float diameter, float ringWidth, float alpha, const Vector2& screenSize)
	{
		int size = std::max((int)std::round(diameter), 1);
		auto* texture = GetTouchImage(imageID, size, ringWidth);
		if (texture == nullptr)
			return;

		auto topLeft = Vector2(std::round(center.x - (size / 2.0f)), std::round(center.y - (size / 2.0f)));
		auto bottomRight = topLeft + Vector2((float)size);

		BindTexture(TextureRegister::ColorMap, texture, SamplerStateRegister::AnisotropicClamp);
		BeginPrimitiveBatch();
		DrawTouchQuad(*_primitiveBatch, topLeft, bottomRight, screenSize, Vector2::Zero, Vector2::One, Vector4(1.0f, 1.0f, 1.0f, alpha));
		EndPrimitiveBatch();
	}

	// A low-res icon, TRX's pixels as solid squares on the white texture. Each pixel gets a whole
	// number of screen pixels, so none blur or come out larger than another.
	void Renderer::DrawTouchGlyphLowRes(const TouchGlyph& glyph, const Vector2& screenSize)
	{
		constexpr auto HALF_CLEAR_ALPHA = 146.0f / 255.0f;

		int index = (int)glyph.Icon;
		if (index < 0 || index >= TOUCH_ICON_COUNT || glyph.Size <= 0.0f)
			return;

		float pixelSize = std::max(1.0f, std::round(glyph.Size / TOUCH_ICON_LOW_RES_SIZE));
		auto origin = Vector2(
			std::round(glyph.Center.x - ((pixelSize * TOUCH_ICON_LOW_RES_SIZE) / 2.0f)),
			std::round(glyph.Center.y - ((pixelSize * TOUCH_ICON_LOW_RES_SIZE) / 2.0f)));

		auto white = Vector4(1.0f, 1.0f, 1.0f, glyph.Alpha);
		auto black = Vector4(0.0f, 0.0f, 0.0f, glyph.Alpha);
		auto halfClearBlack = Vector4(0.0f, 0.0f, 0.0f, glyph.Alpha * HALF_CLEAR_ALPHA);
		auto uv = Vector2(0.5f);

		for (int y = 0; y < TOUCH_ICON_LOW_RES_SIZE; y++)
		{
			const char* row = TOUCH_ICONS_LOW_RES[index][y];

			// One quad for each run of same pixels in a row.
			for (int x = 0; x < TOUCH_ICON_LOW_RES_SIZE;)
			{
				char pixel = row[x];
				int runEnd = x + 1;
				while (runEnd < TOUCH_ICON_LOW_RES_SIZE && row[runEnd] == pixel)
					runEnd++;

				if (pixel != ' ')
				{
					const auto& color = (pixel == '#') ? white : ((pixel == '+') ? halfClearBlack : black);
					auto topLeft = origin + (Vector2((float)x, (float)y) * pixelSize);
					auto bottomRight = origin + (Vector2((float)runEnd, (float)(y + 1)) * pixelSize);
					DrawTouchQuad(*_primitiveBatch, topLeft, bottomRight, screenSize, uv, uv, color);
				}

				x = runEnd;
			}
		}
	}

	// Draws the on-screen touch controls over the finished frame, in game, inventory
	// and menus alike.
	void Renderer::DrawTouchControls()
	{
		auto screenSize = Vector2((float)_graphicsDevice->GetScreenWidth(), (float)_graphicsDevice->GetScreenHeight());
		SetTouchScreenSize((int)screenSize.x, (int)screenSize.y);

		auto drawList = GetTouchDrawList();
		_touchFrame++;

		if (!drawList.Circles.empty() || !drawList.Glyphs.empty())
		{
			_graphicsDevice->BindRenderTarget(_backBuffer->GetRenderTarget(), _backBuffer->GetDepthTarget());
			_graphicsDevice->SetViewport(_viewport);
			_graphicsDevice->SetScissor(_viewport);

			SetBlendMode(BlendMode::AlphaBlend, true);
			SetDepthState(DepthState::None, true);
			SetCullMode(CullMode::None, true);

			_shaders.Bind(Shader::FullScreenQuad);
			_graphicsDevice->SetPrimitiveType(PrimitiveType::TriangleList);
			_graphicsDevice->SetInputLayout(_vertexInputLayout.get());

			for (const auto& circle : drawList.Circles)
				DrawTouchImage(TOUCH_ICON_COUNT + (int)circle.Shape, circle.Center, circle.Radius * 2.0f, circle.RingWidth, circle.Alpha, screenSize);

			if (drawList.IsLowRes)
			{
				BindTexture(TextureRegister::ColorMap, _whiteTexture.get(), SamplerStateRegister::AnisotropicClamp);
				BeginPrimitiveBatch();
				for (const auto& glyph : drawList.Glyphs)
					DrawTouchGlyphLowRes(glyph, screenSize);
				EndPrimitiveBatch();
			}
			else
			{
				for (const auto& glyph : drawList.Glyphs)
					DrawTouchImage((int)glyph.Icon, glyph.Center, glyph.Size, 0.0f, glyph.Alpha, screenSize);
			}
		}

		// Free drawings no longer shown at their size.
		for (auto it = _touchImages.begin(); it != _touchImages.end();)
		{
			if ((_touchFrame - it->second.second) > TOUCH_IMAGE_UNUSED_FRAMES_MAX)
				it = _touchImages.erase(it);
			else
				it++;
		}
	}
}
