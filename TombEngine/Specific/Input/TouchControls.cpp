#include "framework.h"
#include "Specific/Input/TouchControls.h"

#include "Game/Lara/lara.h"
#include "Specific/configuration.h"
#include "Specific/Input/Bindings.h"
#include "Specific/Input/InputAction.h"
#include "Specific/Input/Input.h"
#include "Specific/Input/Keys.h"

namespace TEN::Input
{
	// Layout of TRX's touch controls with sprint and crouch, which TombEngine always has.
	// Offsets and radii are fractions of the screen's shorter side.

	enum class TouchAnchor
	{
		BottomLeft,
		BottomRight,
		TopCenter
	};

	struct TouchButtonDef
	{
		int			Key		= KEY_UNASSIGNED; // TK_STICK_UP for the stick, which takes 4 keys.
		TouchAnchor Anchor	= TouchAnchor::BottomRight;
		float		OffsetX = 0.0f;
		float		OffsetY = 0.0f;
		float		Radius	= 0.0f;
	};

	struct TouchButton
	{
		int		Key		= KEY_UNASSIGNED;
		Vector2 Center	= Vector2::Zero;
		float	Radius	= 0.0f;
		bool	IsStick = false;
		bool	Active	= false;
	};

	struct TouchFinger
	{
		SDL_FingerID ID		  = 0;
		Vector2		 Position = Vector2::Zero; // Normalized to the screen.
		bool		 IsActive = false;
	};

	constexpr auto TOUCH_BUTTON_DEFS = std::array<TouchButtonDef, 11>
	{
		TouchButtonDef{ TK_STICK_UP,  TouchAnchor::BottomLeft,	0.15f, 0.15f, 0.11f },

		TouchButtonDef{ TK_JUMP,	  TouchAnchor::BottomRight, 0.10f, 0.10f, 0.065f },
		TouchButtonDef{ TK_ACTION,	  TouchAnchor::BottomRight, 0.21f, 0.21f, 0.065f },

		TouchButtonDef{ TK_WALK,	  TouchAnchor::BottomRight, 0.32f, 0.21f, 0.035f },
		TouchButtonDef{ TK_LOOK,	  TouchAnchor::BottomRight, 0.29f, 0.13f, 0.035f },
		TouchButtonDef{ TK_ROLL,	  TouchAnchor::BottomRight, 0.21f, 0.10f, 0.035f },
		TouchButtonDef{ TK_DRAW,	  TouchAnchor::BottomRight, 0.21f, 0.32f, 0.035f },
		TouchButtonDef{ TK_SPRINT,	  TouchAnchor::BottomRight, 0.10f, 0.21f, 0.035f },
		TouchButtonDef{ TK_CROUCH,	  TouchAnchor::BottomRight, 0.13f, 0.29f, 0.035f },

		TouchButtonDef{ TK_INVENTORY, TouchAnchor::TopCenter, -0.05f, 0.04f, 0.03f },
		TouchButtonDef{ TK_PAUSE,	  TouchAnchor::TopCenter,  0.05f, 0.04f, 0.03f }
	};

	constexpr auto MAX_FINGERS			= 10;
	constexpr auto STICK_DIRECTION_MIN	= 0.38f; // Of the unit vector towards the finger.
	constexpr auto STICK_THUMB_LIMIT	= 0.68f; // Of the stick's radius.
	constexpr auto STICK_THUMB_RATIO	= 0.25f;
	constexpr auto STICK_BASE_SCALE		= 1.2f;
	constexpr auto HIT_GENEROSITY		= 1.2f;
	constexpr auto FINGER_RADIUS		= 0.020f;
	constexpr auto BORDER_RATIO			= 0.004f;
	constexpr auto ICON_SCALE			= 1.4f;

	// Alphas of TRX's touch controls. The circle SVGs have the colours of a pressed button,
	// which the rest show fainter (TRX: 120 of 210 for the disc, 120 of 220 for the ring).
	constexpr auto CIRCLE_ALPHA			= 120.0f / 215.0f;
	constexpr auto CIRCLE_ACTIVE_ALPHA	= 1.0f;
	constexpr auto ICON_ALPHA			= 113.0f / 255.0f;
	constexpr auto ICON_ACTIVE_ALPHA	= 239.0f / 255.0f;

	static auto Buttons		   = std::vector<TouchButton>{};
	static auto Fingers		   = std::array<TouchFinger, MAX_FINGERS>{};
	static auto KeyStates	   = std::array<bool, TOUCH_KEY_COUNT>{};
	static auto ScreenSize	   = Vector2(800.0f, 600.0f);
	static auto FingerRadius   = 0.0f;
	static auto BorderWidth	   = 0.0f;
	static auto IsVisible	   = false;

	static auto StickFingerID  = SDL_FingerID(0);
	static auto IsStickActive  = false;
	static auto StickThumb	   = Vector2::Zero; // Fractions of the stick's radius.

	// Finger events may come from another thread than the one that reads and draws.
	static auto Mutex = std::mutex{};

	static const GameConfiguration* PreviewConfig = nullptr;


	static const GameConfiguration& GetSettings()
	{
		return (PreviewConfig != nullptr) ? *PreviewConfig : g_Configuration;
	}

	static void SetKeyState(int keyID, bool isPressed)
	{
		int index = keyID - KEY_OFFSET_TOUCH;
		if (index >= 0 && index < TOUCH_KEY_COUNT)
			KeyStates[index] = isPressed;
	}

	static void ComputeLayout()
	{
		Buttons.clear();
		if (ScreenSize.x <= 0.0f || ScreenSize.y <= 0.0f)
			return;

		float ref = std::min(ScreenSize.x, ScreenSize.y);
		float buttonScale = GetSettings().TouchButtonScale;
		float stickScale = GetSettings().TouchStickScale * STICK_BASE_SCALE;

		FingerRadius = FINGER_RADIUS * ref * buttonScale;
		BorderWidth = BORDER_RATIO * ref * buttonScale;

		for (const auto& def : TOUCH_BUTTON_DEFS)
		{
			bool isStick = (def.Key == TK_STICK_UP);
			float scale = isStick ? stickScale : buttonScale;
			auto offset = Vector2(def.OffsetX, def.OffsetY) * ref * scale;

			auto button = TouchButton{};
			button.Key = def.Key;
			button.IsStick = isStick;
			button.Radius = def.Radius * ref * scale;

			switch (def.Anchor)
			{
			case TouchAnchor::BottomLeft:
				button.Center = Vector2(offset.x, ScreenSize.y - offset.y);
				break;

			case TouchAnchor::BottomRight:
				button.Center = Vector2(ScreenSize.x - offset.x, ScreenSize.y - offset.y);
				break;

			case TouchAnchor::TopCenter:
				button.Center = Vector2((ScreenSize.x / 2.0f) + offset.x, offset.y);
				break;
			}

			Buttons.push_back(button);
		}
	}

	static TouchButton* FindStick()
	{
		for (auto& button : Buttons)
		{
			if (button.IsStick)
				return &button;
		}

		return nullptr;
	}

	static bool IsInStick(const TouchButton& stick, const Vector2& point)
	{
		float hitRadius = stick.Radius * HIT_GENEROSITY;
		return (Vector2::DistanceSquared(point, stick.Center) <= (hitRadius * hitRadius));
	}

	static void ResetStick()
	{
		IsStickActive = false;
		StickFingerID = 0;
		StickThumb = Vector2::Zero;

		for (int keyID = TK_STICK_UP; keyID <= TK_STICK_RIGHT; keyID++)
			SetKeyState(keyID, false);
	}

	static void UpdateStick(const TouchButton& stick, const Vector2& point)
	{
		auto delta = point - stick.Center;
		float distance = delta.Length();
		auto direction = (distance > 0.0f) ? (delta / distance) : Vector2::Zero;

		float thumbDistance = std::min(distance, stick.Radius * STICK_THUMB_LIMIT);
		StickThumb = (direction * thumbDistance) / stick.Radius;

		bool isOutsideDeadzone = (distance >= (stick.Radius * GetSettings().TouchStickDeadzone));
		SetKeyState(TK_STICK_UP,	isOutsideDeadzone && direction.y < -STICK_DIRECTION_MIN);
		SetKeyState(TK_STICK_DOWN,	isOutsideDeadzone && direction.y > STICK_DIRECTION_MIN);
		SetKeyState(TK_STICK_LEFT,	isOutsideDeadzone && direction.x < -STICK_DIRECTION_MIN);
		SetKeyState(TK_STICK_RIGHT, isOutsideDeadzone && direction.x > STICK_DIRECTION_MIN);
	}

	// Every finger presses each button its contact circle overlaps, so one finger can
	// press two neighbouring buttons at once.
	static void SyncButtonStates()
	{
		for (auto& button : Buttons)
		{
			if (button.IsStick)
				continue;

			button.Active = false;
			for (const auto& finger : Fingers)
			{
				if (!finger.IsActive || (IsStickActive && finger.ID == StickFingerID))
					continue;

				float hitRadius = (button.Radius * HIT_GENEROSITY) + FingerRadius;
				if (Vector2::DistanceSquared(finger.Position * ScreenSize, button.Center) <= (hitRadius * hitRadius))
				{
					button.Active = true;
					break;
				}
			}

			SetKeyState(button.Key, button.Active);
		}
	}

	static TouchFinger* FindFinger(SDL_FingerID id)
	{
		for (auto& finger : Fingers)
		{
			if (finger.IsActive && finger.ID == id)
				return &finger;
		}

		return nullptr;
	}

	static TouchFinger* AllocateFinger()
	{
		for (auto& finger : Fingers)
		{
			if (!finger.IsActive)
				return &finger;
		}

		return nullptr;
	}

	static void ReleaseAll()
	{
		ResetStick();
		for (auto& button : Buttons)
			button.Active = false;

		Fingers = {};
		KeyStates = {};
	}

	static void SetVisible(bool isVisible)
	{
		IsVisible = isVisible;
		if (!isVisible)
			ReleaseAll();
	}

	static void HandleFingerDown(const SDL_TouchFingerEvent& event)
	{
		auto position = Vector2(event.x, event.y);
		auto point = position * ScreenSize;

		auto* finger = AllocateFinger();
		if (finger == nullptr)
			return;

		finger->ID = event.fingerID;
		finger->Position = position;
		finger->IsActive = true;

		auto* stick = FindStick();
		if (stick != nullptr && !IsStickActive && IsInStick(*stick, point))
		{
			IsStickActive = true;
			StickFingerID = event.fingerID;
			UpdateStick(*stick, point);
		}

		SyncButtonStates();
	}

	static void HandleFingerMotion(const SDL_TouchFingerEvent& event)
	{
		auto* finger = FindFinger(event.fingerID);
		if (finger == nullptr)
			return;

		finger->Position = Vector2(event.x, event.y);

		if (IsStickActive && event.fingerID == StickFingerID)
		{
			auto* stick = FindStick();
			if (stick != nullptr)
				UpdateStick(*stick, finger->Position * ScreenSize);
		}

		SyncButtonStates();
	}

	static void HandleFingerUp(const SDL_TouchFingerEvent& event)
	{
		auto* finger = FindFinger(event.fingerID);
		if (finger != nullptr)
			finger->IsActive = false;

		if (IsStickActive && event.fingerID == StickFingerID)
			ResetStick();

		SyncButtonStates();
	}

	static ActionID GetBoundAction(int keyID)
	{
		for (int i = 0; i < (int)ActionID::Count; i++)
		{
			auto actionID = (ActionID)i;
			if (g_Bindings.GetBoundKeyID(BindingProfileID::Touch, actionID) == keyID)
				return actionID;
		}

		return ActionID::Count;
	}

	// Icon of the action a button has, showing what it does now where that changes:
	// jump swims in water, action shoots with weapons out.
	static TouchIcon GetIcon(int keyID)
	{
		switch (GetBoundAction(keyID))
		{
		case In::Jump:
			if (Lara.Control.WaterStatus == WaterStatus::Underwater ||
				Lara.Control.WaterStatus == WaterStatus::TreadWater ||
				Lara.Control.WaterStatus == WaterStatus::FlyCheat)
			{
				return TouchIcon::Swim;
			}

			return TouchIcon::Jump;

		case In::Action:
			if (Lara.Control.HandStatus == HandStatus::WeaponReady ||
				Lara.Control.HandStatus == HandStatus::WeaponDraw ||
				Lara.Control.HandStatus == HandStatus::WeaponUndraw)
			{
				return TouchIcon::Bullet;
			}

			return TouchIcon::Action;

		case In::Walk:		return TouchIcon::Walk;
		case In::Sprint:	return TouchIcon::Run;
		case In::Crouch:	return TouchIcon::Crouch;
		case In::Draw:		return TouchIcon::DrawWeapon;
		case In::Look:		return TouchIcon::Look;
		case In::Roll:		return TouchIcon::Roll;
		case In::Inventory: return TouchIcon::Menu;
		case In::Pause:		return TouchIcon::Pause;
		default:			return TouchIcon::Count;
		}
	}

	static float WithOpacity(float alpha)
	{
		return std::clamp(alpha * GetSettings().TouchOpacity, 0.0f, 1.0f);
	}

	void InitializeTouchControls()
	{
		auto lock = std::lock_guard<std::mutex>(Mutex);
		Buttons.clear();
		ReleaseAll();
		SetVisible(g_Configuration.EnableTouchControls);
	}

	void ApplyTouchConfiguration()
	{
		auto lock = std::lock_guard<std::mutex>(Mutex);
		SetVisible(g_Configuration.EnableTouchControls);
	}

	bool HandleTouchEvent(const SDL_Event& event)
	{
		if (event.type != SDL_EVENT_FINGER_DOWN && event.type != SDL_EVENT_FINGER_MOTION &&
			event.type != SDL_EVENT_FINGER_UP && event.type != SDL_EVENT_FINGER_CANCELED)
		{
			return false;
		}

		auto lock = std::lock_guard<std::mutex>(Mutex);

		// A touch on a touchscreen turns the controls on, as in TRX. That touch only shows them:
		// the player hasn't seen yet what is under the finger.
		if (event.type == SDL_EVENT_FINGER_DOWN && !g_Configuration.EnableTouchControls &&
			SDL_GetTouchDeviceType(event.tfinger.touchID) == SDL_TOUCH_DEVICE_DIRECT)
		{
			g_Configuration.EnableTouchControls = true;
			SetVisible(true);
			SaveConfiguration();
			return true;
		}

		if (!IsVisible)
			return false;

		ComputeLayout();
		switch (event.type)
		{
		case SDL_EVENT_FINGER_DOWN:
			HandleFingerDown(event.tfinger);
			break;

		case SDL_EVENT_FINGER_MOTION:
			HandleFingerMotion(event.tfinger);
			break;

		default:
			HandleFingerUp(event.tfinger);
			break;
		}

		return true;
	}

	void SetTouchPreview(const GameConfiguration* config)
	{
		auto lock = std::lock_guard<std::mutex>(Mutex);
		PreviewConfig = config;
	}

	bool IsTouchKeyHeld(int keyID)
	{
		auto lock = std::lock_guard<std::mutex>(Mutex);
		int index = keyID - KEY_OFFSET_TOUCH;
		if (!IsVisible || index < 0 || index >= TOUCH_KEY_COUNT)
			return false;

		return KeyStates[index];
	}

	bool AreTouchControlsVisible()
	{
		auto lock = std::lock_guard<std::mutex>(Mutex);
		return IsVisible;
	}

	bool IsAnyFingerDown()
	{
		auto lock = std::lock_guard<std::mutex>(Mutex);
		for (const auto& finger : Fingers)
		{
			if (finger.IsActive)
				return true;
		}

		return false;
	}

	void SetTouchScreenSize(int width, int height)
	{
		auto lock = std::lock_guard<std::mutex>(Mutex);
		ScreenSize = Vector2((float)width, (float)height);
	}

	TouchDrawList GetTouchDrawList()
	{
		auto lock = std::lock_guard<std::mutex>(Mutex);
		auto list = TouchDrawList{};
		if (!IsVisible)
			return list;

		list.IsLowRes = GetSettings().TouchLowResIcons;

		ComputeLayout();
		SyncButtonStates();

		for (const auto& button : Buttons)
		{
			if (button.IsStick)
			{
				float deadzoneRadius = button.Radius * GetSettings().TouchStickDeadzone;

				list.Circles.push_back({ TouchShape::Button, button.Center, button.Radius, BorderWidth, WithOpacity(CIRCLE_ALPHA) });
				list.Circles.push_back({ TouchShape::Ring, button.Center, deadzoneRadius, BorderWidth, WithOpacity(CIRCLE_ALPHA) });
				list.Circles.push_back({ TouchShape::Thumb, button.Center + (StickThumb * button.Radius), button.Radius * STICK_THUMB_RATIO, 0.0f, WithOpacity(1.0f) });
				continue;
			}

			list.Circles.push_back({ TouchShape::Button, button.Center, button.Radius, BorderWidth, WithOpacity(button.Active ? CIRCLE_ACTIVE_ALPHA : CIRCLE_ALPHA) });

			auto icon = GetIcon(button.Key);
			if (icon != TouchIcon::Count)
				list.Glyphs.push_back({ button.Center, button.Radius * ICON_SCALE, icon, WithOpacity(button.Active ? ICON_ACTIVE_ALPHA : ICON_ALPHA) });
		}

		return list;
	}

	void SetTouchBinding(ActionID actionID, int keyID)
	{
		for (const auto& group : ACTION_ID_GROUPS)
		{
			if (std::find(group.begin(), group.end(), actionID) == group.end())
				continue;

			for (auto otherActionID : group)
			{
				if (otherActionID != actionID && g_Bindings.GetBoundKeyID(BindingProfileID::Touch, otherActionID) == keyID)
					g_Bindings.SetKeyBinding(BindingProfileID::Touch, otherActionID, KEY_UNASSIGNED);
			}
		}

		g_Bindings.SetKeyBinding(BindingProfileID::Touch, actionID, keyID);
	}

	void RestoreTouchBindings(const BindingProfile& savedBindings)
	{
		g_Bindings.SetDefaultBindingProfile(BindingProfileID::Touch);
		for (const auto& [actionID, keyID] : savedBindings)
			g_Bindings.SetKeyBinding(BindingProfileID::Touch, actionID, keyID);
	}
}
