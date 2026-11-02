#include "pch.h"
#include "Shared/Video/SoftwareRenderer.h"
#include "Shared/Video/VideoRenderer.h"
#include "Shared/RenderedFrame.h"
#include "Shared/Emulator.h"
#include "Shared/NotificationManager.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

// AOF2 experimental presentation-only zoom for Mesen 2's software renderer.
// Opt in with MESEN_AOF2_ZOOM=1.25 (1.0 disables zoom).
// Gameplay simulation, sprite/VRAM data, and emulated HUD remain untouched.
// NOTE: The protected bands are provisional, not verified AOF2 pixel bounds.
static double GetAof2Zoom()
{
	const char* value = std::getenv("MESEN_AOF2_ZOOM");
	if(!value || !*value) { return 1.0; }
	char* end = nullptr;
	double zoom = std::strtod(value, &end);
	if(end == value || *end != '\0' || !std::isfinite(zoom)) { return 1.0; }
	return std::max(1.0, std::min(zoom, 1.6));
}

static void ZoomAof2Frame(uint32_t* dst, const uint32_t* src, uint32_t width, uint32_t height, double zoom)
{
	if(zoom <= 1.0 || width == 0 || height <= 2) {
		std::copy(src, src + size_t(width) * height, dst);
		return;
	}

	// Keep the top HUD band (health, timer, etc.) unscaled.
	// Preserve the bottom strip too. Both are adjustable after game validation.
	const uint32_t top = std::min<uint32_t>(40, height / 4);
	const uint32_t bottom = std::min<uint32_t>(24, height / 4);
	const uint32_t playHeight = height - top - bottom;
	std::copy(src, src + size_t(width) * height, dst);

	const double centerX = (double(width) - 1.0) / 2.0;
	const double centerY = double(top) + (double(playHeight) - 1.0) / 2.0;
	for(uint32_t y = top; y < height - bottom; ++y) {
		const double sy = centerY + (double(y) - centerY) / zoom;
		const uint32_t sourceY = std::min(height - bottom - 1,
			std::max(top, uint32_t(std::floor(sy + 0.5))));
		for(uint32_t x = 0; x < width; ++x) {
			const double sx = centerX + (double(x) - centerX) / zoom;
			const uint32_t sourceX = std::min(width - 1,
				uint32_t(std::max(0.0, std::floor(sx + 0.5))));
			dst[size_t(y) * width + x] = src[size_t(sourceY) * width + sourceX];
		}
	}
}

SoftwareRenderer::SoftwareRenderer(Emulator* emu)
{
	_emu = emu;
	SetScreenSize(256, 240);
	_emu->GetVideoRenderer()->RegisterRenderingDevice(this);
}

SoftwareRenderer::~SoftwareRenderer()
{
	delete[] _textureBuffer[0];
	delete[] _textureBuffer[1];
}

void SoftwareRenderer::SetScreenSize(uint32_t width, uint32_t height)
{
	_frameWidth = width;
	_frameHeight = height;

	delete[] _textureBuffer[0];
	delete[] _textureBuffer[1];
	_textureBuffer[0] = new uint32_t[width * height];
	_textureBuffer[1] = new uint32_t[width * height];
	memset(_textureBuffer[0], 0, width * height * sizeof(uint32_t));
	memset(_textureBuffer[1], 0, width * height * sizeof(uint32_t));
}

void SoftwareRenderer::UpdateFrame(RenderedFrame& frame)
{
	if(_frameWidth != frame.Width || _frameHeight != frame.Height) {
		auto lock = _frameLock.AcquireSafe();
		if(_frameWidth != frame.Width || _frameHeight != frame.Height) {
			SetScreenSize(frame.Width, frame.Height);
		}
	}

	auto lock = _textureLock.AcquireSafe();
	const double zoom = GetAof2Zoom();
	if(zoom > 1.0) {
		ZoomAof2Frame(_textureBuffer[0], frame.FrameBuffer, frame.Width, frame.Height, zoom);
	} else {
		memcpy(_textureBuffer[0], frame.FrameBuffer, frame.Width * frame.Height * sizeof(uint32_t));
	}
	_needSwap = true;
}

void SoftwareRenderer::ClearFrame()
{
	//Clear current output and display black frame
	auto lock = _textureLock.AcquireSafe();
	memset(_textureBuffer[0], 0, _frameWidth * _frameHeight * sizeof(uint32_t));
	_needSwap = true;
}

struct SoftwareRendererSurface
{
	uint32_t* Buffer = nullptr;
	uint32_t Width = 0;
	uint32_t Height = 0;
	bool IsDirty = true;
};

struct SoftwareRendererFrame
{
	SoftwareRendererSurface Frame;
	SoftwareRendererSurface EmuHud;
	SoftwareRendererSurface ScriptHud;
};

void SoftwareRenderer::Render(RenderSurfaceInfo& emuHud, RenderSurfaceInfo& scriptHud)
{
	auto lock = _frameLock.AcquireSafe();
	
	if(_needSwap) {
		_needSwap = false;
		auto textureLock = _textureLock.AcquireSafe();
		std::swap(_textureBuffer[0], _textureBuffer[1]);
	}

	SoftwareRendererFrame frame = {
		{ _textureBuffer[1], _frameWidth, _frameHeight, true },
		{ emuHud.Buffer, emuHud.Width, emuHud.Height, emuHud.IsDirty },
		{ scriptHud.Buffer, scriptHud.Width, scriptHud.Height, scriptHud.IsDirty }
	};

	_emu->GetNotificationManager()->SendNotification(ConsoleNotificationType::RefreshSoftwareRenderer, &frame);
}

void SoftwareRenderer::Reset()
{
}

void SoftwareRenderer::SetExclusiveFullscreenMode(bool fullscreen, void* windowHandle)
{
	//not supported
}
