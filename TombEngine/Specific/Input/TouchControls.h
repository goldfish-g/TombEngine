#pragma once

#include <SDL3/SDL_events.h>

#include "Specific/Input/Bindings.h"

struct GameConfiguration;

namespace TEN::Input
{
	// On-screen touch controls: a virtual stick and round buttons the engine draws over
	// the game, in the layout of TRX's touch controls. Each place on the overlay is a key
	// (see TouchKey in Keys.h), so actions bind to it like to any other key.

	enum class TouchIcon
	{
		Jump,
		Crouch,
		Walk,
		Run,
		DrawWeapon,
		Look,
		Action,
		Roll,
		Menu,
		Pause,
		Bullet,
		Swim,

		Count
	};

	// The circles of the controls, each an SVG (see Tools/TouchIcons).
	enum class TouchShape
	{
		Button, // A button, also the D-pad's base.
		Ring,	// The D-pad's deadzone.
		Thumb,	// The D-pad's thumb.

		Count
	};

	struct TouchCircle
	{
		TouchShape Shape	 = TouchShape::Button;
		Vector2	   Center	 = Vector2::Zero; // Screen pixels.
		float	   Radius	 = 0.0f;
		float	   RingWidth = 0.0f;		   // Of the SVG's "ring", in screen pixels.
		float	   Alpha	 = 1.0f;		   // Applied to the SVG's colours.
	};

	struct TouchGlyph
	{
		Vector2	  Center = Vector2::Zero;
		float	  Size	 = 0.0f; // Side of the square the icon fits in.
		TouchIcon Icon	 = TouchIcon::Count;
		float	  Alpha	 = 1.0f;
	};

	struct TouchDrawList
	{
		std::vector<TouchCircle> Circles = {};
		std::vector<TouchGlyph>	 Glyphs	 = {};
		bool					 IsLowRes = false; // TRX's pixel icons.
	};

	void InitializeTouchControls();
	void ApplyTouchConfiguration();

	// Settings to show the controls with instead of the saved ones (nullptr to stop), for
	// the settings page. Turning the controls on or off still waits for the saved settings.
	void SetTouchPreview(const GameConfiguration* config);

	// Finger events; true if the overlay used the event.
	bool HandleTouchEvent(const SDL_Event& event);

	// Whether a touch key (see TouchKey in Keys.h) is held now.
	bool IsTouchKeyHeld(int keyID);

	bool AreTouchControlsVisible();
	bool IsAnyFingerDown();

	// Size of the screen the overlay is laid out on, in pixels.
	void SetTouchScreenSize(int width, int height);
	TouchDrawList GetTouchDrawList();

	// Binds a touch key to an action. A touch button does one action of each action group,
	// so another action of the group it did loses it.
	void SetTouchBinding(ActionID actionID, int keyID);

	// The default touch bindings with saved ones over them.
	void RestoreTouchBindings(const BindingProfile& savedBindings);
}
