-- ScreenMatchmaking overlay — osu!-style queue + opponent-found sequence.
--
-- Presentation layer for the engine's matchmaking flow. The C++ screen
-- (ScreenMatchmaking) drives state, timing, input and sound; this file
-- only draws. Driven by MATCHMAKING:GetState() polling and the
-- MatchmakingMatched MESSAGEMAN event.
--
-- Themes can override this file by providing the same path:
--   BGAnimations/ScreenMatchmaking overlay/default.lua
--
-- NOTE: in this engine, Def tables stored in Lua locals do NOT become
-- actor proxies — all runtime access goes through self:GetChild(...).

-- ---------------------------------------------------------------- design
-- tokens (edit these to restyle the whole sequence)

local COLORS = {
	bg       = color("#05070F"),
	flash    = color("#FFFFFF"),
	vs       = color("#FFFFFF"),
	you      = color("#3D94FF"),   -- fallback "you" accent
	opponent = color("#FF577E"),   -- osu!-ish red
	ring     = color("#FFCC22"),   -- osu! queue ring yellow
	text     = color("#FFFFFF"),
	subtext  = color("#9AA4B8"),
	panel    = color("#0C1420"),
}

-- Pull the accent palette from Simply Love when it is the active theme:
-- "you" gets the user's theme color, the opponent keeps the red.
if SL ~= nil and SL.Global ~= nil and SL.Global.Colors ~= nil then
	local idx = SL.Global.ActiveColorIndex or 1
	if SL.Global.Colors[idx] ~= nil then
		COLORS.you = color(SL.Global.Colors[idx])
	end
end

local FONT = "Common Normal"

local T = {
	stagger    = 0.10,
	fadein     = 0.20,
	flash      = 0.40,
	vs_zoom    = 1.00,
	slide      = 0.50,
	slide_dist = 400,
	drift      = 4,
	fling_at   = 4.60,  -- cards hold on screen until here (photo moment)
	fling      = 0.40,  -- engine transitions at 5.0s, right as they leave
}

-- ---------------------------------------------------------------- helpers

local function display_name()
	if GAMESTATE:IsPlayerEnabled(PLAYER_1) then
		return GAMESTATE:GetPlayerDisplayName(PLAYER_1)
	end
	return GAMESTATE:GetPlayerDisplayName(PLAYER_2)
end

local function initial_of(name)
	name = tostring(name or "?")
	return name:sub(1, 1):upper()
end

-- Load the local player's avatar into a player card (same treatment the
-- opponent card gets), if the player has one.
local function load_local_icon(card)
	local path = MATCHMAKING:GetLocalIconPath()
	if path ~= nil and path ~= "" then
		card:GetChild("Icon"):Load(path)
		card:GetChild("Icon"):SetSize(88, 88)
		card:GetChild("Icon"):visible(true)
		card:GetChild("Ring"):visible(false)
	end
end

-- A player "card": accent bar + ring/initial (or icon) + Label + Country chip
-- + Name, all addressable via GetChild.
local function PlayerCard(accent, fallback_initial)
	local card_w, card_h = 380, 120
	local avatar_x = -card_w / 2 + 16 + 44
	local name_x = -card_w / 2 + 128

	return Def.ActorFrame{
		Def.Quad{
			InitCommand = function(self)
				self:SetSize(card_w, card_h)
				self:diffuse(COLORS.panel)
				self:diffusealpha(0.92)
			end,
		},
		Def.Quad{
			InitCommand = function(self)
				self:SetSize(6, card_h)
				self:x(-card_w / 2 + 3)
				self:diffuse(accent)
			end,
		},
		Def.ActorFrame{
			Name = "Ring",
			Def.Quad{
				InitCommand = function(self)
					self:SetSize(96, 96)
					self:xy(avatar_x, 0)
					self:diffuse(COLORS.ring)
				end,
			},
			Def.Quad{
				InitCommand = function(self)
					self:SetSize(88, 88)
					self:xy(avatar_x, 0)
					self:diffuse(COLORS.panel)
				end,
			},
			Def.BitmapText{
				Name = "Letter",
				Font = FONT,
				Text = fallback_initial,
				InitCommand = function(self)
					self:xy(avatar_x, -2)
					self:zoom(1.6)
					self:diffuse(COLORS.ring)
				end,
			},
		},
		Def.Sprite{
			Name = "Icon",
			InitCommand = function(self)
				self:SetSize(88, 88)
				self:xy(avatar_x, 0)
				self:visible(false)
			end,
		},
		Def.BitmapText{
			Name = "Label",
			Font = FONT,
			Text = "",
			InitCommand = function(self)
				self:xy(name_x, -24)
				self:zoom(0.7)
				self:diffuse(accent)
				self:halign(0)
			end,
		},
		-- country chip, shown only when the server knows the country
		Def.ActorFrame{
			Name = "Chip",
			InitCommand = function(self)
				self:xy(name_x + 22, 14)
				self:visible(false)
			end,
			Def.Quad{
				InitCommand = function(self)
					self:SetSize(46, 30)
					self:diffuse(accent)
					self:diffusealpha(0.35)
				end,
			},
			Def.Quad{
				InitCommand = function(self)
					self:SetSize(42, 26)
					self:diffuse(COLORS.panel)
				end,
			},
			Def.BitmapText{
				Name = "Code",
				Font = FONT,
				Text = "",
				InitCommand = function(self)
					self:zoom(0.65)
					self:diffuse(accent)
				end,
			},
		},
		Def.BitmapText{
			Name = "Name",
			Font = FONT,
			Text = "",
			InitCommand = function(self)
				self:xy(name_x, 14)
				self:zoom(1.1)
				self:maxwidth(220)
				self:diffuse(COLORS.text)
				self:halign(0)
			end,
		},
	}
end

-- ---------------------------------------------------------------- actors

local elapsed = 0
local found_start = nil
local found_triggered = false
local opp_base_x = SCREEN_CENTER_X * 0.55
local you_base_x = SCREEN_CENTER_X * 1.45

local root = Def.ActorFrame{ Name = "ScreenMatchmakingOverlay" }

root[#root + 1] = Def.Quad{
	Name = "BG",
	InitCommand = function(self)
		self:FullScreen()
		self:diffuse(COLORS.bg)
		self:diffusealpha(0.96)
	end,
}

-- drifting accent shapes (osu!-triangles vibe), wrapped in Update()
local shapes = Def.ActorFrame{ Name = "Shapes" }
local NUM_SHAPES = 12
for i = 1, NUM_SHAPES do
	local accent = (i % 2 == 0) and COLORS.you or COLORS.opponent
	shapes[#shapes + 1] = Def.Quad{
		Name = "Shape" .. i,
		InitCommand = function(self)
			local size = math.random(18, 46)
			self:SetSize(size, size)
			self:xy(math.random(0, SCREEN_WIDTH), math.random(0, SCREEN_HEIGHT))
			self:rotationz(45)
			self:diffuse(accent)
			self:diffusealpha(math.random(3, 9) / 100)
		end,
	}
end
local shape_speed = {}
for i = 1, NUM_SHAPES do
	shape_speed[i] = math.random(14, 44) * (i % 2 == 0 and 1 or -1)
end
root[#root + 1] = shapes

-- ------------------------------------------------------- waiting scene
local waiting_group = Def.ActorFrame{ Name = "Waiting" }
root[#root + 1] = waiting_group

waiting_group[#waiting_group + 1] = Def.BitmapText{
	Name = "Title",
	Font = FONT,
	Text = "SEARCHING FOR A MATCH",
	InitCommand = function(self)
		self:xy(SCREEN_CENTER_X, SCREEN_CENTER_Y - 120)
		self:zoom(1.4)
		self:diffuse(COLORS.text)
		self:diffusealpha(0)
		self:decelerate(T.fadein)
		self:diffusealpha(1)
	end,
}

waiting_group[#waiting_group + 1] = Def.BitmapText{
	Name = "Timer",
	Font = FONT,
	Text = "0:00",
	InitCommand = function(self)
		self:xy(SCREEN_CENTER_X, SCREEN_CENTER_Y - 72)
		self:zoom(0.9)
		self:diffuse(COLORS.subtext)
		self:diffusealpha(0)
		self:sleep(T.stagger)
		self:decelerate(T.fadein)
		self:diffusealpha(1)
	end,
}

waiting_group[#waiting_group + 1] = Def.Quad{
	Name = "Spinner",
	InitCommand = function(self)
		self:SetSize(22, 22)
		self:xy(SCREEN_CENTER_X, SCREEN_CENTER_Y - 16)
		self:diffuse(COLORS.you)
		self:diffusealpha(0)
		self:sleep(T.stagger * 2)
		self:decelerate(T.fadein)
		self:diffusealpha(1)
		self:queuecommand("Spin")
	end,
	SpinCommand = function(self)
		self:linear(1.2)
		self:rotationz(360)
		self:queuecommand("Spin")
	end,
}

waiting_group[#waiting_group + 1] = PlayerCard(COLORS.you, initial_of(display_name())) .. {
	Name = "OwnCard",
	InitCommand = function(self)
		self:xy(SCREEN_CENTER_X, SCREEN_CENTER_Y + 110)
		self:diffusealpha(0)
		self:sleep(T.stagger * 3)
		self:decelerate(T.fadein)
		self:diffusealpha(1)
		self:GetChild("Label"):settext("YOU")
		self:GetChild("Name"):settext(display_name())
		load_local_icon(self)
	end,
}

waiting_group[#waiting_group + 1] = Def.BitmapText{
	Name = "Hint",
	Font = FONT,
	Text = "START: play solo    BACK: cancel",
	InitCommand = function(self)
		self:xy(SCREEN_CENTER_X, SCREEN_CENTER_Y + 220)
		self:zoom(0.6)
		self:diffuse(COLORS.subtext)
		self:diffusealpha(0)
		self:sleep(T.stagger * 4)
		self:decelerate(T.fadein)
		self:diffusealpha(1)
	end,
}

-- --------------------------------------------------------- found scene
local found_group = Def.ActorFrame{
	Name = "Found",
	InitCommand = function(self) self:visible(false) end,
}
root[#root + 1] = found_group

found_group[#found_group + 1] = Def.Quad{
	Name = "WipeTop",
	InitCommand = function(self)
		self:SetSize(SCREEN_WIDTH, SCREEN_HEIGHT / 2)
		self:xy(SCREEN_CENTER_X, SCREEN_HEIGHT / 4)
		self:diffuse(COLORS.opponent)
		self:diffusealpha(0)
	end,
}

found_group[#found_group + 1] = Def.Quad{
	Name = "WipeBottom",
	InitCommand = function(self)
		self:SetSize(SCREEN_WIDTH, SCREEN_HEIGHT / 2)
		self:xy(SCREEN_CENTER_X, SCREEN_HEIGHT * 3 / 4)
		self:diffuse(COLORS.you)
		self:diffusealpha(0)
	end,
}

found_group[#found_group + 1] = PlayerCard(COLORS.opponent, "?") .. {
	Name = "OppCard",
	InitCommand = function(self)
		self:xy(opp_base_x, SCREEN_CENTER_Y - 40)
		self:visible(false)
	end,
}

found_group[#found_group + 1] = PlayerCard(COLORS.you, initial_of(display_name())) .. {
	Name = "YouCard",
	InitCommand = function(self)
		self:xy(you_base_x, SCREEN_CENTER_Y + 80)
		self:visible(false)
		self:GetChild("Label"):settext("YOU")
		self:GetChild("Name"):settext(display_name())
		load_local_icon(self)
	end,
}

found_group[#found_group + 1] = Def.BitmapText{
	Name = "VS",
	Font = FONT,
	Text = "VS",
	InitCommand = function(self)
		self:xy(SCREEN_CENTER_X, SCREEN_CENTER_Y)
		self:zoom(2.0)
		self:diffuse(COLORS.vs)
		self:visible(false)
	end,
}

found_group[#found_group + 1] = Def.Quad{
	Name = "Flash",
	InitCommand = function(self)
		self:FullScreen()
		self:diffuse(COLORS.flash)
		self:diffusealpha(0)
		self:visible(false)
	end,
	HideCommand = function(self) self:visible(false) end,
}

-- ------------------------------------------------------- found trigger

local function StartFound(self, params)
	if found_triggered then
		return
	end
	found_triggered = true

	local waiting = self:GetChild("Waiting")
	local found = self:GetChild("Found")
	local opp = found:GetChild("OppCard")
	local you = found:GetChild("YouCard")
	local flash = found:GetChild("Flash")
	local vs = found:GetChild("VS")

	local opp_name = (params and params.name) or MATCHMAKING:GetOpponentName()
	opp:GetChild("Label"):settext("OPPONENT")
	opp:GetChild("Name"):settext(opp_name)
	opp:GetChild("Ring"):GetChild("Letter"):settext(initial_of(opp_name))

	local country = MATCHMAKING:GetOpponentCountry()
	if country ~= nil and country ~= "" then
		opp:GetChild("Chip"):GetChild("Code"):settext(country)
		opp:GetChild("Chip"):visible(true)
		-- make room for the chip at the start of the name line
		opp:GetChild("Name"):addx(52)
	end

	local opp_icon = MATCHMAKING:GetOpponentIconPath()
	if opp_icon ~= nil and opp_icon ~= "" then
		opp:GetChild("Icon"):Load(opp_icon)
		opp:GetChild("Icon"):SetSize(88, 88)
		opp:GetChild("Icon"):visible(true)
		opp:GetChild("Ring"):visible(false)
	end

	waiting:stoptweening()
	waiting:accelerate(0.25)
	waiting:diffusealpha(0)
	found:visible(true)

	flash:visible(true)
	flash:diffusealpha(1)
	flash:decelerate(T.flash)
	flash:diffusealpha(0)
	flash:sleep(T.flash)
	flash:queuecommand("Hide")

	vs:visible(true)
	vs:zoom(2.0)
	vs:diffusealpha(1)
	vs:decelerate(T.vs_zoom)
	vs:zoom(0.5)

	found:GetChild("WipeTop"):diffusealpha(0.14)
	found:GetChild("WipeBottom"):diffusealpha(0.14)

	opp:stoptweening()
	opp:visible(true)
	opp:addx(-T.slide_dist)
	opp:decelerate(T.slide)
	opp:addx(T.slide_dist)

	you:stoptweening()
	you:visible(true)
	you:addx(T.slide_dist)
	you:decelerate(T.slide)
	you:addx(-T.slide_dist)
end

local function FlingOut(self)
	local found = self:GetChild("Found")
	local opp = found:GetChild("OppCard")
	local you = found:GetChild("YouCard")
	local vs = found:GetChild("VS")

	opp:stoptweening()
	opp:accelerate(T.fling)
	opp:addx(-SCREEN_WIDTH * 0.8)

	you:stoptweening()
	you:accelerate(T.fling)
	you:addx(SCREEN_WIDTH * 0.8)

	vs:accelerate(T.fling)
	vs:zoom(0.25)
	vs:diffusealpha(0)
end

-- ------------------------------------------------------------- update

local function Update(self, dt)
	elapsed = elapsed + dt

	-- animated backdrop, always alive
	for i = 1, NUM_SHAPES do
		local s = self:GetChild("Shapes"):GetChild("Shape" .. i)
		local nx = s:GetX() + shape_speed[i] * dt
		if shape_speed[i] > 0 and nx > SCREEN_WIDTH + 60 then
			nx = -60
		elseif shape_speed[i] < 0 and nx < -60 then
			nx = SCREEN_WIDTH + 60
		end
		s:x(nx)
	end

	if MATCHMAKING == nil then
		return
	end

	if not found_triggered then
		local waiting = self:GetChild("Waiting")
		waiting:GetChild("Title"):diffusealpha(0.6 + 0.4 * math.sin(elapsed * 4))
		local e = MATCHMAKING:GetSearchElapsed()
		waiting:GetChild("Timer"):settext(
			string.format("%d:%02d", math.floor(e / 60), math.floor(e % 60)))
		if MATCHMAKING:GetState() == "Matched" then
			StartFound(self, nil)
		end
		return
	end

	if found_start == nil then
		found_start = elapsed
	end
	local ft = elapsed - found_start
	local found = self:GetChild("Found")

	-- wipes breathe during the whole found scene
	found:GetChild("WipeTop"):diffusealpha(0.11 + 0.05 * math.sin(ft * 3))
	found:GetChild("WipeBottom"):diffusealpha(0.11 + 0.05 * math.sin(ft * 3 + 1.5))

	if ft >= T.slide and ft < T.fling_at then
		local d = math.sin((ft - T.slide) * 1.5) * T.drift
		found:GetChild("OppCard"):x(opp_base_x + d)
		found:GetChild("YouCard"):x(you_base_x - d)
	end

	if ft >= T.fling_at and ft - dt < T.fling_at then
		FlingOut(self)
	end
end

root.InitCommand = function(self)
	self:SetUpdateFunction(Update)
end

root.MatchmakingMatchedMessageCommand = function(self, params)
	StartFound(self, params)
end

return root
