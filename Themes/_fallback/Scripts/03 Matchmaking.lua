-- Engine-level "Search Opponent" option row for ScreenPlayerOptions.
-- Degrades gracefully when the engine has no MATCHMAKING global.
function OptionRowSearchOpponent()
	local t = {
		Name = "SearchOpponent";
		LayoutType = "ShowOneInRow";
		SelectType = "SelectOne";
		OneChoiceForAllPlayers = true;
		ExportOnChange = false;
		Choices = { 'Off','On' };
		LoadSelections = function(self, list, pn)
			if MATCHMAKING ~= nil and MATCHMAKING:IsSearchEnabled() and GAMESTATE:GetNumSidesJoined() == 1 then
				list[2] = true;
			else
				list[1] = true;
			end;
		end;
		SaveSelections = function(self, list, pn)
			if MATCHMAKING ~= nil then
				MATCHMAKING:SetSearchEnabled(list[2] == true and GAMESTATE:GetNumSidesJoined() == 1);
			end;
		end;
	};
	return t;
end;
