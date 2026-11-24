// ToypadRemote: control Cemu's emulated LEGO Dimensions Toy Pad from a phone browser.
//
// - Serves the web app from <cemu>/resources/toypad-remote
// - REST endpoints to place / remove / move / create figures (.bin files in the figures folder)
// - Server-Sent Events stream with slot contents and the LED colours the game sends to the pad
//
// Config: <user data>/toypad_remote.txt  (key=value lines: port=8090, figures=<folder>)

#include "ToypadRemote.h"
#include "Dimensions.h"

#include "nsyshid.h"
#include "Common/FileStream.h"
#include "config/ActiveSettings.h"
#include "Cafe/HW/Latte/Core/LatteOverlay.h"

#ifdef _WIN32
#include <winsock2.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#endif

#include "httplib.h"

#include <condition_variable>
#include <fstream>

namespace nsyshid::ToypadRemote
{
	// pad number (1 = centre, 2 = left, 3 = right) of each slot index, same layout as Cemu's UI
	static constexpr std::array<uint8, 7> SLOT_PAD = {2, 1, 3, 2, 2, 3, 3};
	// slots of each pad (index = pad - 1), 255 = unused
	static constexpr std::array<std::array<uint8, 3>, 3> PAD_SLOTS = {{{1, 255, 255}, {0, 3, 4}, {2, 5, 6}}};
	static constexpr std::array<const char*, 3> PAD_NAME = {"Centro", "Esquerda", "Direita"};

	struct Led
	{
		char mode = 'c'; // c = colour, f = fade, F = flash, r = random fade
		uint8 r = 0, g = 0, b = 0;
		uint8 p1 = 0, p2 = 0, p3 = 0; // fade: speed, cycles | flash: on, off, cycles
		uint32 seq = 0;				  // bumped on every animation so the client restarts it
	};

	static std::mutex s_mutex;
	static std::condition_variable s_cv;
	static uint64 s_version = 1;
	static std::array<Led, 3> s_leds;
	static std::array<std::string, 7> s_slotFile; // file name loaded in each slot (by this remote)
	static bool s_awake = false;
	static sint32 s_selected = -1; // slot the controller shortcuts act on
	static fs::path s_figuresDir;
	static std::string s_url;
	static std::unique_ptr<httplib::Server> s_server;

	static void Notify() // caller holds s_mutex
	{
		s_version++;
		s_cv.notify_all();
	}

	// ---------------------------------------------------------------- LED capture

	static void SetLed(uint8 pad, char mode, uint8 r, uint8 g, uint8 b, uint8 p1 = 0, uint8 p2 = 0, uint8 p3 = 0)
	{
		auto apply = [&](Led& l) {
			if (mode == 'c' && l.mode == 'c' && l.r == r && l.g == g && l.b == b)
				return false;
			l.mode = mode;
			l.r = r, l.g = g, l.b = b;
			l.p1 = p1, l.p2 = p2, l.p3 = p3;
			l.seq++;
			return true;
		};
		bool changed = false;
		if (pad == 0)
		{
			for (auto& l : s_leds)
				changed |= apply(l);
		}
		else if (pad <= 3)
			changed = apply(s_leds[pad - 1]);
		if (changed)
			Notify();
	}

	void OnToypadCommand(std::span<const uint8, 32> buf)
	{
		const uint8 cmd = buf[2];
		const uint8* p = buf.data() + 4; // payload
		if (cmd < 0xB0 || cmd > 0xC8)
			return;
		std::lock_guard lock(s_mutex);
		switch (cmd)
		{
		case 0xB0: // wake: the game (re)connected to the pad
			s_awake = true;
			Notify();
			break;
		case 0xC0: // colour: pad, r, g, b
			SetLed(p[0], 'c', p[1], p[2], p[3]);
			break;
		case 0xC2: // fade: pad, speed, cycles, r, g, b
			SetLed(p[0], 'f', p[3], p[4], p[5], p[1], p[2]);
			break;
		case 0xC3: // flash: pad, on, off, cycles, r, g, b
			SetLed(p[0], 'F', p[4], p[5], p[6], p[1], p[2], p[3]);
			break;
		case 0xC4: // fade random: pad, speed, cycles
			SetLed(p[0], 'r', 0, 0, 0, p[1], p[2]);
			break;
		case 0xC6: // fade all: per pad [enable, speed, cycles, r, g, b]
			for (uint8 i = 0; i < 3; i++)
			{
				const uint8* q = p + i * 6;
				if (q[0])
					SetLed(i + 1, 'f', q[3], q[4], q[5], q[1], q[2]);
			}
			break;
		case 0xC7: // flash all: per pad [enable, on, off, cycles, r, g, b]
			for (uint8 i = 0; i < 3; i++)
			{
				const uint8* q = p + i * 7;
				if (q[0])
					SetLed(i + 1, 'F', q[4], q[5], q[6], q[1], q[2], q[3]);
			}
			break;
		case 0xC8: // colour all: per pad [enable, r, g, b]
			for (uint8 i = 0; i < 3; i++)
			{
				const uint8* q = p + i * 4;
				if (q[0])
					SetLed(i + 1, 'c', q[1], q[2], q[3]);
			}
			break;
		default:
			break;
		}
	}

	// ---------------------------------------------------------------- helpers

	static std::string JsonEscape(std::string_view s)
	{
		std::string out;
		out.reserve(s.size() + 2);
		for (unsigned char c : s)
		{
			if (c == '"' || c == '\\')
			{
				out += '\\';
				out += (char)c;
			}
			else if (c < 0x20)
				out += fmt::format("\\u{:04x}", c);
			else
				out += (char)c;
		}
		return out;
	}

	static std::string StateJson() // caller holds s_mutex
	{
		const auto slots = g_dimensionstoypad.GetSlots();
		// boot changes on every Cemu start so open pages can reload themselves after an update
		static const uint64 s_boot = (uint64)std::chrono::system_clock::now().time_since_epoch().count();
		std::string j = fmt::format("{{\"boot\":\"{}\",\"v\":{},\"awake\":{},\"sel\":{},\"slots\":[", s_boot, s_version, s_awake ? "true" : "false", s_selected);
		for (size_t i = 0; i < slots.size(); i++)
		{
			if (i)
				j += ',';
			if (!slots[i])
			{
				s_slotFile[i].clear(); // emptied elsewhere (e.g. Cemu's own window)
				j += "null";
			}
			else
				j += fmt::format("{{\"id\":{},\"file\":\"{}\"}}", *slots[i], JsonEscape(s_slotFile[i]));
		}
		j += "],\"leds\":[";
		for (size_t i = 0; i < s_leds.size(); i++)
		{
			const Led& l = s_leds[i];
			if (i)
				j += ',';
			j += fmt::format("{{\"m\":\"{}\",\"c\":[{},{},{}],\"p\":[{},{},{}],\"s\":{}}}", l.mode, l.r, l.g, l.b, l.p1, l.p2, l.p3, l.seq);
		}
		j += "]}";
		return j;
	}

	// only plain "<name>.bin" files directly inside the figures folder are accepted
	static std::optional<fs::path> FigurePath(const std::string& name)
	{
		if (name.empty() || name.size() > 200 || name.find_first_of("/\\:*?\"<>|") != std::string::npos || name.find("..") != std::string::npos)
			return std::nullopt;
		fs::path p = s_figuresDir / _utf8ToPath(name);
		if (p.extension() != ".bin")
			return std::nullopt;
		return p;
	}

	static bool ReadFigure(const fs::path& path, std::array<uint8, 0x2D * 0x04>& data)
	{
		std::ifstream f(path, std::ios::binary);
		return f && f.read((char*)data.data(), data.size()) && f.gcount() == (std::streamsize)data.size();
	}

	static std::optional<uint8> SlotParam(const httplib::Request& req, const char* key)
	{
		if (!req.has_param(key))
			return std::nullopt;
		const std::string v = req.get_param_value(key);
		if (v.size() != 1 || v[0] < '0' || v[0] > '6')
			return std::nullopt;
		return (uint8)(v[0] - '0');
	}

	static void Fail(httplib::Response& res, int status, std::string_view msg)
	{
		res.status = status;
		res.set_content(fmt::format("{{\"error\":\"{}\"}}", JsonEscape(msg)), "application/json");
	}

	static void Ok(httplib::Response& res)
	{
		res.set_content("{\"ok\":true}", "application/json");
	}

	// ---------------------------------------------------------------- figure actions (caller holds s_mutex)

	static void MoveSlot(uint8 from, uint8 to)
	{
		g_dimensionstoypad.TempRemove(from);
		g_dimensionstoypad.MoveFigure(SLOT_PAD[to], to, SLOT_PAD[from], from);
		if (from != to)
		{
			s_slotFile[to] = std::move(s_slotFile[from]);
			s_slotFile[from].clear();
		}
		s_selected = to;
	}

	static std::string PlaceFigure(uint8 slot, const std::string& name)
	{
		const auto path = FigurePath(name);
		if (!path)
			return "invalid file name";

		// a tag can only be on one spot: if it is already on the pad, move it
		const auto slots = g_dimensionstoypad.GetSlots();
		for (uint8 i = 0; i < 7; i++)
		{
			if (slots[i] && s_slotFile[i] == name)
			{
				if (i != slot)
					MoveSlot(i, slot);
				return {};
			}
		}

		std::unique_ptr<FileStream> file(FileStream::openFile2(*path, true));
		if (!file)
			return "figure file not found";
		std::array<uint8, 0x2D * 0x04> data;
		if (file->readData(data.data(), data.size()) != data.size())
			return "figure file is not a 180 byte dump";

		if (slots[slot])
			g_dimensionstoypad.RemoveFigure(SLOT_PAD[slot], slot, true);
		g_dimensionstoypad.LoadFigure(data, std::move(file), SLOT_PAD[slot], slot);
		s_slotFile[slot] = name;
		s_selected = slot;
		return {};
	}

	static void RemoveSlot(uint8 slot)
	{
		g_dimensionstoypad.RemoveFigure(SLOT_PAD[slot], slot, true);
		s_slotFile[slot].clear();
		if (s_selected == slot)
			s_selected = -1;
	}

	// moves the figure in `from` to the zone of `pad` (1 centre, 2 left, 3 right): first free spot there,
	// otherwise it swaps places with the figure on the zone's first spot. Returns the new slot or -1.
	static sint32 MoveToZone(uint8 from, uint8 pad)
	{
		const auto slots = g_dimensionstoypad.GetSlots();
		if (!slots[from] || pad < 1 || pad > 3)
			return -1;
		if (SLOT_PAD[from] == pad)
			return from;
		for (uint8 s : PAD_SLOTS[pad - 1])
		{
			if (s != 255 && !slots[s])
			{
				MoveSlot(from, s);
				return s;
			}
		}
		const uint8 target = PAD_SLOTS[pad - 1][0];
		if (!g_dimensionstoypad.SwapFigures(SLOT_PAD[from], from, SLOT_PAD[target], target))
			return -1;
		std::swap(s_slotFile[from], s_slotFile[target]);
		s_selected = target;
		return target;
	}

	static std::string SlotName(uint8 slot)
	{
		const auto slots = g_dimensionstoypad.GetSlots();
		if (!slots[slot])
			return "(vazio)";
		if (!s_slotFile[slot].empty())
			return s_slotFile[slot].substr(0, s_slotFile[slot].size() - 4); // drop ".bin"
		return g_dimensionstoypad.FindFigure(*slots[slot]);
	}

	// ---------------------------------------------------------------- loadouts (<figures>/loadouts.txt: name<TAB>file0<TAB>...file6)

	struct Loadout
	{
		std::string name;
		std::array<std::string, 7> files;
	};

	static std::vector<Loadout> ReadLoadouts()
	{
		std::vector<Loadout> list;
		std::ifstream in(s_figuresDir / "loadouts.txt");
		std::string line;
		while (std::getline(in, line))
		{
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			if (line.empty())
				continue;
			Loadout l;
			size_t start = 0;
			for (int field = 0; field < 8; field++)
			{
				const size_t tab = line.find('\t', start);
				const std::string value = line.substr(start, tab == std::string::npos ? std::string::npos : tab - start);
				if (field == 0)
					l.name = value;
				else
					l.files[field - 1] = value;
				if (tab == std::string::npos)
					break;
				start = tab + 1;
			}
			list.push_back(std::move(l));
		}
		return list;
	}

	static void WriteLoadouts(const std::vector<Loadout>& list)
	{
		std::ofstream out(s_figuresDir / "loadouts.txt", std::ios::trunc);
		for (const Loadout& l : list)
		{
			out << l.name;
			for (const auto& f : l.files)
				out << '\t' << f;
			out << '\n';
		}
	}

	// ---------------------------------------------------------------- library (<figures>/favorites.txt: one file name per line)

	static std::set<std::string> ReadFavorites()
	{
		std::set<std::string> favs;
		std::ifstream in(s_figuresDir / "favorites.txt");
		std::string line;
		while (std::getline(in, line))
		{
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			if (!line.empty())
				favs.insert(line);
		}
		return favs;
	}

	static void WriteFavorites(const std::set<std::string>& favs)
	{
		std::ofstream out(s_figuresDir / "favorites.txt", std::ios::trunc);
		for (const auto& f : favs)
			out << f << '\n';
	}

	static bool IsOnPad(const std::string& file)
	{
		const auto slots = g_dimensionstoypad.GetSlots();
		for (uint8 i = 0; i < 7; i++)
			if (slots[i] && s_slotFile[i] == file)
				return true;
		return false;
	}

	// a figure file was renamed (to = new name) or deleted (to = ""): update favourites and loadouts
	static void RenameReferences(const std::string& from, const std::string& to)
	{
		auto favs = ReadFavorites();
		if (favs.erase(from) && !to.empty())
			favs.insert(to);
		WriteFavorites(favs);
		auto list = ReadLoadouts();
		for (auto& l : list)
			for (auto& f : l.files)
				if (f == from)
					f = to;
		WriteLoadouts(list);
	}

	static std::string ApplyLoadout(const Loadout& l)
	{
		// take off everything that is not part of the loadout, then put each figure on its spot
		const auto slots = g_dimensionstoypad.GetSlots();
		for (uint8 i = 0; i < 7; i++)
		{
			if (slots[i] && std::find(l.files.begin(), l.files.end(), s_slotFile[i]) == l.files.end())
				RemoveSlot(i);
		}
		std::string firstError;
		for (uint8 i = 0; i < 7; i++)
		{
			if (l.files[i].empty())
			{
				if (g_dimensionstoypad.GetSlots()[i])
					RemoveSlot(i);
				continue;
			}
			if (s_slotFile[i] == l.files[i] && g_dimensionstoypad.GetSlots()[i])
				continue;
			const std::string err = PlaceFigure(i, l.files[i]);
			if (!err.empty() && firstError.empty())
				firstError = l.files[i] + ": " + err;
		}
		s_selected = -1;
		return firstError;
	}

	// ---------------------------------------------------------------- controller shortcuts

	bool OnGamepad(bool modifierHeld, uint32 hold)
	{
		static uint32 s_prevHold = 0;
		static bool s_prevModifier = false;
		enum : uint32
		{
			BTN_A = 0x8000, BTN_B = 0x4000, BTN_X = 0x2000, BTN_Y = 0x1000,
			BTN_L = 0x0020, BTN_R = 0x0010,
			BTN_UP = 0x0200, BTN_DOWN = 0x0100, BTN_LEFT = 0x0800, BTN_RIGHT = 0x0400,
		};

		// only buttons pressed while Home is already held count (holding A then pressing Home does nothing)
		const uint32 pressed = (modifierHeld && s_prevModifier) ? (hold & ~s_prevHold) : 0;
		const bool justHeld = modifierHeld && !s_prevModifier;
		s_prevHold = hold;
		s_prevModifier = modifierHeld;
		if (!modifierHeld)
			return false;
		if (!pressed && !justHeld)
			return true;

		std::string msg;
		{
			std::lock_guard lock(s_mutex);
			const auto slots = g_dimensionstoypad.GetSlots();
			auto occupied = [&](sint32 s) { return s >= 0 && s < 7 && slots[s].has_value(); };
			auto cycle = [&](int dir) {
				// order around the pad: left (0,3,4), centre (1), right (2,5,6)
				static constexpr std::array<uint8, 7> ORDER = {0, 3, 4, 1, 2, 5, 6};
				sint32 pos = -1;
				for (sint32 i = 0; i < 7; i++)
					if (ORDER[i] == s_selected)
						pos = i;
				for (int step = 1; step <= 7; step++)
				{
					const uint8 s = ORDER[(pos + dir * step + 7 * 7) % 7];
					if (slots[s])
					{
						s_selected = s;
						return;
					}
				}
			};
			if (!occupied(s_selected))
			{
				s_selected = -1;
				cycle(1);
			}

			if (pressed & (BTN_DOWN | BTN_R))
				cycle(1);
			else if (pressed & BTN_L)
				cycle(-1);

			const uint8 zonePad = (pressed & BTN_LEFT) ? 2 : (pressed & BTN_UP) ? 1 : (pressed & BTN_RIGHT) ? 3 : 0;
			const sint32 loadoutIndex = (pressed & BTN_A) ? 0 : (pressed & BTN_B) ? 1 : (pressed & BTN_X) ? 2 : (pressed & BTN_Y) ? 3 : -1;

			if (loadoutIndex >= 0)
			{
				const auto list = ReadLoadouts();
				if (loadoutIndex < (sint32)list.size())
				{
					const std::string err = ApplyLoadout(list[loadoutIndex]);
					msg = err.empty() ? "Toy Pad: loadout \"" + list[loadoutIndex].name + "\"" : "Toy Pad: " + err;
				}
				else
					msg = fmt::format("Toy Pad: sem loadout {}", loadoutIndex + 1);
			}
			else if (s_selected < 0)
				msg = "Toy Pad: nenhuma figura no pad";
			else if (zonePad)
			{
				const std::string name = SlotName((uint8)s_selected);
				const sint32 to = MoveToZone((uint8)s_selected, zonePad);
				msg = to >= 0 ? fmt::format("Toy Pad: {} -> {}", name, PAD_NAME[zonePad - 1]) : "Toy Pad: nao foi possivel mover";
			}
			else
				msg = fmt::format("Toy Pad: {} ({})  |  setas: mover  L/R: escolher  A/B/X/Y: loadout",
								  SlotName((uint8)s_selected), PAD_NAME[SLOT_PAD[s_selected] - 1]);
			Notify();
		}
		LatteOverlay_pushNotification(msg, 2500);
		return true;
	}

	static std::string SanitizeName(std::string_view in)
	{
		std::string out;
		for (unsigned char c : in)
		{
			if (std::isalnum(c) || c == ' ' || c == '-' || c == '_' || c == '\'' || c == '(' || c == ')' || c >= 0x80)
				out += (char)c;
		}
		while (!out.empty() && (out.back() == '.' || out.back() == ' '))
			out.pop_back();
		if (out.size() > 80)
			out.resize(80);
		return out;
	}

	// ---------------------------------------------------------------- config / network

	static void LoadConfig(int& port)
	{
		const fs::path cfgPath = ActiveSettings::GetUserDataPath("toypad_remote.txt");
		s_figuresDir = ActiveSettings::GetUserDataPath("dimensions");
		std::ifstream in(cfgPath);
		if (!in)
		{
			std::ofstream out(cfgPath);
			out << "# Toy Pad Remote settings (restart Cemu after editing)\n";
			out << "port=" << port << "\n";
			out << "figures=" << _pathToUtf8(s_figuresDir) << "\n";
		}
		std::string line;
		while (std::getline(in, line))
		{
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			const size_t eq = line.find('=');
			if (line.empty() || line[0] == '#' || eq == std::string::npos)
				continue;
			const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
			if (key == "port")
				port = std::clamp(std::atoi(value.c_str()), 1, 65535);
			else if (key == "figures" && !value.empty())
				s_figuresDir = _utf8ToPath(value);
		}
		std::error_code ec;
		fs::create_directories(s_figuresDir, ec);
	}

	// IPv4 of an up adapter in Tailscale's 100.64.0.0/10 range (empty if none), so phones off the LAN can connect
	static std::string TailscaleAddress()
	{
#ifdef _WIN32
		ULONG size = 16 * 1024;
		std::vector<uint8> buffer(size);
		auto* addrs = (IP_ADAPTER_ADDRESSES*)buffer.data();
		if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_MULTICAST, nullptr, addrs, &size) == ERROR_BUFFER_OVERFLOW)
		{
			buffer.resize(size);
			addrs = (IP_ADAPTER_ADDRESSES*)buffer.data();
			if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_MULTICAST, nullptr, addrs, &size) != NO_ERROR)
				return {};
		}
		for (auto* a = addrs; a; a = a->Next)
		{
			if (a->OperStatus != IfOperStatusUp)
				continue;
			for (auto* u = a->FirstUnicastAddress; u; u = u->Next)
			{
				auto* sin = (sockaddr_in*)u->Address.lpSockaddr;
				const uint32 ip = ntohl(sin->sin_addr.s_addr);
				if ((ip & 0xFFC00000) != 0x64400000) // 100.64.0.0/10
					continue;
				char text[INET_ADDRSTRLEN]{};
				if (inet_ntop(AF_INET, &sin->sin_addr, text, sizeof(text)))
					return text;
			}
		}
#endif
		return {};
	}

	static std::string LanAddress()
	{
#ifdef _WIN32
		// prefer an adapter that is up and has a default gateway (the real LAN, not VPN/VM adapters)
		ULONG size = 16 * 1024;
		std::vector<uint8> buffer(size);
		auto* addrs = (IP_ADAPTER_ADDRESSES*)buffer.data();
		if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_MULTICAST, nullptr, addrs, &size) == ERROR_BUFFER_OVERFLOW)
		{
			buffer.resize(size);
			addrs = (IP_ADAPTER_ADDRESSES*)buffer.data();
			if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_MULTICAST, nullptr, addrs, &size) != NO_ERROR)
				return "localhost";
		}
		for (auto* a = addrs; a; a = a->Next)
		{
			if (a->OperStatus != IfOperStatusUp || !a->FirstGatewayAddress || !a->FirstUnicastAddress)
				continue;
			auto* sin = (sockaddr_in*)a->FirstUnicastAddress->Address.lpSockaddr;
			char ip[INET_ADDRSTRLEN]{};
			if (inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip)))
				return ip;
		}
#endif
		return "localhost";
	}

	std::string GetUrl()
	{
		std::lock_guard lock(s_mutex);
		return s_url;
	}

	// ---------------------------------------------------------------- server

	void Start()
	{
		if (s_server)
			return;
		int port = 8090;
		LoadConfig(port);

		s_server = std::make_unique<httplib::Server>();
		httplib::Server& srv = *s_server;

		const fs::path webDir = ActiveSettings::GetDataPath("resources/toypad-remote");
		srv.set_mount_point("/", _pathToUtf8(webDir));
		srv.set_file_request_handler([](const httplib::Request& req, httplib::Response& res) {
			// html must always be fresh, the rest (catalog, icons) can be cached
			if (req.path == "/" || req.path.ends_with(".html"))
				res.set_header("Cache-Control", "no-cache");
			else
				res.set_header("Cache-Control", "max-age=86400");
		});

		srv.Get("/api/state", [](const httplib::Request&, httplib::Response& res) {
			std::lock_guard lock(s_mutex);
			res.set_header("Cache-Control", "no-store");
			res.set_content(StateJson(), "application/json");
		});

		srv.Get("/api/events", [](const httplib::Request&, httplib::Response& res) {
			res.set_header("Cache-Control", "no-store");
			auto lastSent = std::make_shared<uint64>(0);
			res.set_chunked_content_provider("text/event-stream", [lastSent](size_t, httplib::DataSink& sink) {
				std::string msg;
				{
					std::unique_lock lock(s_mutex);
					s_cv.wait_for(lock, std::chrono::seconds(10), [&] { return s_version != *lastSent; });
					if (s_version != *lastSent)
					{
						*lastSent = s_version;
						msg = "data: " + StateJson() + "\n\n";
					}
					else
						msg = ": ping\n\n"; // keep-alive so phones notice dead connections
				}
				return sink.write(msg.data(), msg.size());
			});
		});

		srv.Get("/api/figures", [](const httplib::Request&, httplib::Response& res) {
			fs::path dir;
			std::set<std::string> favs;
			{
				std::lock_guard lock(s_mutex);
				dir = s_figuresDir;
				favs = ReadFavorites();
			}
			std::string j = "[";
			std::error_code ec;
			for (const auto& entry : fs::directory_iterator(dir, ec))
			{
				if (!entry.is_regular_file(ec) || entry.path().extension() != ".bin")
					continue;
				std::array<uint8, 0x2D * 0x04> data;
				const bool ok = ReadFigure(entry.path(), data);
				const std::string name = _pathToUtf8(entry.path().filename());
				if (j.size() > 1)
					j += ',';
				j += fmt::format("{{\"file\":\"{}\",\"id\":{},\"fav\":{}}}", JsonEscape(name),
								 ok ? static_cast<long long>(g_dimensionstoypad.IdentifyFigure(data)) : -1LL,
								 favs.contains(name) ? "true" : "false");
			}
			j += ']';
			res.set_header("Cache-Control", "no-store");
			res.set_content(j, "application/json");
		});

		// ?file=<name.bin>&on=0|1
		srv.Post("/api/figures/fav", [](const httplib::Request& req, httplib::Response& res) {
			const std::string file = req.get_param_value("file");
			if (!FigurePath(file))
				return Fail(res, 400, "invalid file name");
			std::lock_guard lock(s_mutex);
			auto favs = ReadFavorites();
			if (req.get_param_value("on") == "1")
				favs.insert(file);
			else
				favs.erase(file);
			WriteFavorites(favs);
			Ok(res);
		});

		// ?file=<name.bin>&to=<new name, without .bin>; the figure must not be on the pad
		srv.Post("/api/figures/rename", [](const httplib::Request& req, httplib::Response& res) {
			const std::string file = req.get_param_value("file");
			const std::string to = SanitizeName(req.get_param_value("to")) + ".bin";
			const auto from = FigurePath(file), dest = FigurePath(to);
			if (!from || !dest || to == ".bin")
				return Fail(res, 400, "invalid name");
			std::lock_guard lock(s_mutex);
			if (IsOnPad(file))
				return Fail(res, 400, "tira a figura do pad primeiro");
			std::error_code ec;
			if (fs::exists(*dest, ec))
				return Fail(res, 400, "ja existe uma figura com esse nome");
			fs::rename(*from, *dest, ec);
			if (ec)
				return Fail(res, 500, "could not rename");
			RenameReferences(file, to);
			res.set_content(fmt::format("{{\"ok\":true,\"file\":\"{}\"}}", JsonEscape(to)), "application/json");
		});

		// moves the file to <figures>/_apagadas (recoverable) and forgets it in favourites/loadouts
		srv.Post("/api/figures/delete", [](const httplib::Request& req, httplib::Response& res) {
			const std::string file = req.get_param_value("file");
			const auto from = FigurePath(file);
			if (!from)
				return Fail(res, 400, "invalid file name");
			std::lock_guard lock(s_mutex);
			if (IsOnPad(file))
				return Fail(res, 400, "tira a figura do pad primeiro");
			std::error_code ec;
			const fs::path bin = s_figuresDir / "_apagadas";
			fs::create_directories(bin, ec);
			fs::path dest = bin / _utf8ToPath(file);
			for (int n = 2; fs::exists(dest, ec); n++)
				dest = bin / _utf8ToPath(fmt::format("{} ({}).bin", file.substr(0, file.size() - 4), n));
			fs::rename(*from, dest, ec);
			if (ec)
				return Fail(res, 500, "could not delete");
			RenameReferences(file, "");
			Ok(res);
		});

		srv.Post("/api/place", [](const httplib::Request& req, httplib::Response& res) {
			const auto slot = SlotParam(req, "slot");
			if (!slot || !req.has_param("file"))
				return Fail(res, 400, "slot (0-6) and file required");
			std::lock_guard lock(s_mutex);
			const std::string err = PlaceFigure(*slot, req.get_param_value("file"));
			if (!err.empty())
				return Fail(res, 400, err);
			Notify();
			Ok(res);
		});

		srv.Post("/api/remove", [](const httplib::Request& req, httplib::Response& res) {
			const auto slot = SlotParam(req, "slot");
			if (!slot)
				return Fail(res, 400, "slot (0-6) required");
			std::lock_guard lock(s_mutex);
			RemoveSlot(*slot);
			Notify();
			Ok(res);
		});

		// ?slot=<0-6>&pad=<1 centre|2 left|3 right>
		srv.Post("/api/zone", [](const httplib::Request& req, httplib::Response& res) {
			const auto slot = SlotParam(req, "slot");
			const int pad = std::atoi(req.get_param_value("pad").c_str());
			if (!slot || pad < 1 || pad > 3)
				return Fail(res, 400, "slot (0-6) and pad (1-3) required");
			std::lock_guard lock(s_mutex);
			if (MoveToZone(*slot, (uint8)pad) < 0)
				return Fail(res, 400, "no figure in that slot");
			Notify();
			Ok(res);
		});

		srv.Get("/api/loadouts", [](const httplib::Request&, httplib::Response& res) {
			std::lock_guard lock(s_mutex);
			std::string j = "[";
			for (const Loadout& l : ReadLoadouts())
			{
				if (j.size() > 1)
					j += ',';
				j += fmt::format("{{\"name\":\"{}\",\"slots\":[", JsonEscape(l.name));
				for (size_t i = 0; i < l.files.size(); i++)
					j += fmt::format("{}\"{}\"", i ? "," : "", JsonEscape(l.files[i]));
				j += "]}";
			}
			j += ']';
			res.set_header("Cache-Control", "no-store");
			res.set_content(j, "application/json");
		});

		// saves the current pad as ?name=<name> (replaces a loadout with the same name)
		srv.Post("/api/loadouts/save", [](const httplib::Request& req, httplib::Response& res) {
			std::string name = req.get_param_value("name");
			std::erase_if(name, [](char c) { return c == '\t' || c == '\r' || c == '\n'; });
			if (name.empty() || name.size() > 60)
				return Fail(res, 400, "name required");
			std::lock_guard lock(s_mutex);
			Loadout l{name};
			const auto slots = g_dimensionstoypad.GetSlots();
			for (uint8 i = 0; i < 7; i++)
				l.files[i] = slots[i] ? s_slotFile[i] : "";
			auto list = ReadLoadouts();
			auto it = std::find_if(list.begin(), list.end(), [&](const Loadout& x) { return x.name == name; });
			if (it != list.end())
				*it = l;
			else
				list.push_back(l);
			WriteLoadouts(list);
			Ok(res);
		});

		srv.Post("/api/loadouts/apply", [](const httplib::Request& req, httplib::Response& res) {
			const size_t i = (size_t)std::atoi(req.get_param_value("i").c_str());
			std::lock_guard lock(s_mutex);
			const auto list = ReadLoadouts();
			if (i >= list.size())
				return Fail(res, 400, "unknown loadout");
			const std::string err = ApplyLoadout(list[i]);
			Notify();
			if (!err.empty())
				return Fail(res, 400, err);
			Ok(res);
		});

		srv.Post("/api/loadouts/delete", [](const httplib::Request& req, httplib::Response& res) {
			const size_t i = (size_t)std::atoi(req.get_param_value("i").c_str());
			std::lock_guard lock(s_mutex);
			auto list = ReadLoadouts();
			if (i >= list.size())
				return Fail(res, 400, "unknown loadout");
			list.erase(list.begin() + i);
			WriteLoadouts(list);
			Ok(res);
		});

		// moves a loadout up one place (?i=) so it can be reached by a different controller button
		srv.Post("/api/loadouts/up", [](const httplib::Request& req, httplib::Response& res) {
			const size_t i = (size_t)std::atoi(req.get_param_value("i").c_str());
			std::lock_guard lock(s_mutex);
			auto list = ReadLoadouts();
			if (i == 0 || i >= list.size())
				return Fail(res, 400, "cannot move");
			std::swap(list[i], list[i - 1]);
			WriteLoadouts(list);
			Ok(res);
		});

		srv.Post("/api/move", [](const httplib::Request& req, httplib::Response& res) {
			const auto from = SlotParam(req, "from"), to = SlotParam(req, "to");
			if (!from || !to)
				return Fail(res, 400, "from and to (0-6) required");
			std::lock_guard lock(s_mutex);
			const auto slots = g_dimensionstoypad.GetSlots();
			if (!slots[*from])
				return Fail(res, 400, "no figure in that slot");
			if (*from != *to && slots[*to])
			{
				// dropping onto another figure swaps the two instead of knocking it off the pad
				g_dimensionstoypad.SwapFigures(SLOT_PAD[*from], *from, SLOT_PAD[*to], *to);
				std::swap(s_slotFile[*from], s_slotFile[*to]);
				s_selected = *to;
			}
			else
				MoveSlot(*from, *to);
			Notify();
			Ok(res);
		});

		// creates a new tag file; ?id=<figure id>&name=<display name>[&slot=<0-6> to place it right away]
		srv.Post("/api/create", [](const httplib::Request& req, httplib::Response& res) {
			const uint32 id = (uint32)std::strtoul(req.get_param_value("id").c_str(), nullptr, 10);
			if (!req.has_param("id") || id > 0xFFFF)
				return Fail(res, 400, "id required");
			std::string base = SanitizeName(req.get_param_value("name"));
			if (base.empty())
				base = fmt::format("Figure {}", id);

			std::lock_guard lock(s_mutex);
			std::string name = base + ".bin";
			std::error_code ec;
			for (int n = 2; fs::exists(s_figuresDir / _utf8ToPath(name), ec); n++)
				name = fmt::format("{} ({}).bin", base, n);
			const fs::path path = s_figuresDir / _utf8ToPath(name);

			// a ready-made template (templates/<id>.bin) wins over the generated blank figure
			const fs::path tpl = ActiveSettings::GetDataPath(fmt::format("resources/toypad-remote/templates/{}.bin", id));
			bool created;
			if (fs::exists(tpl, ec))
				created = fs::copy_file(tpl, path, ec);
			else
				created = id >= 1000 ? g_dimensionstoypad.CreateVehicle(path, id) : g_dimensionstoypad.CreateFigure(path, id);
			if (!created)
				return Fail(res, 500, "could not create the figure file");

			if (const auto slot = SlotParam(req, "slot"))
			{
				const std::string err = PlaceFigure(*slot, name);
				if (!err.empty())
					return Fail(res, 500, err);
				Notify();
			}
			res.set_content(fmt::format("{{\"ok\":true,\"file\":\"{}\"}}", JsonEscape(name)), "application/json");
		});

		if (!srv.bind_to_port("0.0.0.0", port))
		{
			cemuLog_log(LogType::Force, "ToypadRemote: could not listen on port {}", port);
			s_server.reset();
			return;
		}
		{
			std::lock_guard lock(s_mutex);
			s_url = fmt::format("http://{}:{}", LanAddress(), port);
			if (const std::string ts = TailscaleAddress(); !ts.empty())
				s_url += fmt::format("  |  Tailscale: http://{}:{}", ts, port);
		}
		cemuLog_log(LogType::Force, "ToypadRemote: open {} on your phone (figures: {})", s_url, _pathToUtf8(s_figuresDir));
		std::thread([] { s_server->listen_after_bind(); }).detach();
	}
} // namespace nsyshid::ToypadRemote
