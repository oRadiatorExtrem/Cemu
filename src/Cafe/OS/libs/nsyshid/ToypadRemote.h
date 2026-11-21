#pragma once

#include <span>
#include <string>

// Small LAN web server that lets a phone control the emulated LEGO Dimensions Toy Pad
// and mirrors the pad LED colours sent by the game.
namespace nsyshid::ToypadRemote
{
	void Start();
	// called from DimensionsUSB::SendCommand with every 32 byte command the game sends
	void OnToypadCommand(std::span<const uint8, 32> buf);
	// e.g. "http://192.168.1.20:8090" (empty when the server is not running)
	std::string GetUrl();
	// called from VPADController::VPADRead with the emulated GamePad buttons and whether the Home mapping is held.
	// Home + buttons are Toy Pad shortcuts; returns true when the buttons must be hidden from the game.
	bool OnGamepad(bool modifierHeld, uint32 hold);
} // namespace nsyshid::ToypadRemote
