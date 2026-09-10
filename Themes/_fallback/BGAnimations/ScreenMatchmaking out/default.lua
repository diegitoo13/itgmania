-- ScreenMatchmaking out transition: fade the whole screen to black so the
-- handoff into ScreenGameplay is seamless (VS intro -> black -> gameplay
-- entrance) and the theme's background never flashes in between.
return Def.Quad{
	InitCommand = function(self)
		self:FullScreen()
		self:diffuse(color("#000000"))
		self:diffusealpha(0)
		self:decelerate(0.25)
		self:diffusealpha(1)
	end,
}
