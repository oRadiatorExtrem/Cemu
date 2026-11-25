// Adaptador do nucleo partilhado (toypad-remote-core) para o Cemu.
//
// Toda a logica HTTP/SSE/JSON/loadouts vive no nucleo, copiado para
// third_party/toypad-remote-core por `python sync.py`. Este ficheiro e so a ponte para o
// g_dimensionstoypad.
//
// Pontos de integracao no Cemu (os mesmos de antes):
//   1. CemuApp.cpp            -> nsyshid::ToypadRemote::Start()
//   2. Dimensions.cpp:548     -> nsyshid::ToypadRemote::OnToypadCommand(buf)
//   3. VPADController.cpp:81  -> nsyshid::ToypadRemote::OnGamepad(modifierHeld, hold)

#include "ToypadRemote.h"

#include "Dimensions.h"

#include "ToypadRemoteCore.h" // nucleo partilhado

#include "config/ActiveSettings.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace nsyshid::ToypadRemote
{
	namespace
	{
		// Ordem de locks (a mesma que o nucleo documenta): o do nucleo primeiro, este segundo.
		// Nunca os dois sentidos. Snapshot() copia e larga antes de voltar.
		std::mutex s_stateMutex;
		// Nome do ficheiro carregado em cada posicao (0..6). As posicoes em si vivem no
		// g_dimensionstoypad; isto so guarda o nome que o utilizador ve e pode renomear.
		std::array<std::string, 7> s_slotFile;
		bool s_started = false;

		constexpr std::array<uint8, 7> kPadOfSlot = {2, 1, 3, 2, 2, 3, 3};

		class CemuHost final : public toypad_remote::ToypadHost
		{
		  public:
			toypad_remote::PortalSnapshot Snapshot() override
			{
				toypad_remote::PortalSnapshot snap;
				const auto slots = g_dimensionstoypad.GetSlots();
				{
					std::lock_guard lock(s_stateMutex); // copiar e largar: nada e formatado com o lock na mao
					for (size_t i = 0; i < slots.size() && i < snap.slots.size(); i++)
					{
						if (slots[i])
						{
							snap.slots[i].present = true;
							snap.slots[i].id = *slots[i];
							snap.slots[i].file = s_slotFile[i];
						}
					}
				}
				snap.session_ready = true; // o Cemu nao expoe o estado da sessao
				snap.awake = true;
				return snap;
			}

			bool LoadTag(int pad, int index, const toypad_remote::Tag& tag, const std::string& file,
						 uint32_t* out_id, std::string& err) override
			{
				if (index < 0 || index >= 7 || pad < 1 || pad > 3)
				{
					err = "bad slot";
					return false;
				}
				std::unique_ptr<FileStream> stream(FileStream::openFile2(
					ActiveSettings::GetUserDataPath(fmt::format("dimensions/{}", file)), true));
				const uint32 id = g_dimensionstoypad.LoadFigure(tag, std::move(stream), (uint8)pad, (uint8)index);
				if (out_id)
					*out_id = id;
				{
					std::lock_guard lock(s_stateMutex);
					s_slotFile[index] = file;
				}
				return true;
			}

			bool RemoveIndex(int index, std::string& err) override
			{
				if (index < 0 || index >= 7)
				{
					err = "bad slot";
					return false;
				}
				const bool ok = g_dimensionstoypad.RemoveFigure(kPadOfSlot[index], (uint8)index, true);
				{
					std::lock_guard lock(s_stateMutex);
					s_slotFile[index].clear();
				}
				return ok;
			}

			bool MoveIndex(int pad, int index, int old_index, std::string& err) override
			{
				if (index < 0 || index >= 7 || old_index < 0 || old_index >= 7)
				{
					err = "bad slot";
					return false;
				}
				g_dimensionstoypad.TempRemove((uint8)old_index);
				if (!g_dimensionstoypad.MoveFigure((uint8)pad, (uint8)index, kPadOfSlot[old_index], (uint8)old_index))
				{
					err = "could not move";
					return false;
				}
				{
					std::lock_guard lock(s_stateMutex);
					if (old_index != index)
					{
						s_slotFile[index] = std::move(s_slotFile[old_index]);
						s_slotFile[old_index].clear();
					}
				}
				return true;
			}

			bool Swap(int pad_a, int index_a, int pad_b, int index_b, std::string& err) override
			{
				if (!g_dimensionstoypad.SwapFigures((uint8)pad_a, (uint8)index_a, (uint8)pad_b, (uint8)index_b))
				{
					err = "could not swap";
					return false;
				}
				{
					std::lock_guard lock(s_stateMutex);
					std::swap(s_slotFile[index_a], s_slotFile[index_b]);
				}
				return true;
			}

			bool CreateFigure(const std::string& path, uint32 id, std::string& err) override
			{
				if (!g_dimensionstoypad.CreateFigure(_utf8ToPath(path), id))
				{
					err = "could not create the figure file";
					return false;
				}
				return true;
			}

			bool CreateVehicle(const std::string& path, uint32 id, std::string& err) override
			{
				if (!g_dimensionstoypad.CreateVehicle(_utf8ToPath(path), id))
				{
					err = "could not create the vehicle file";
					return false;
				}
				return true;
			}

			void Announce(const std::vector<toypad_remote::Packet>& packets) override
			{
				for (const auto& packet : packets)
				{
					std::array<uint8, 32> buf{};
					std::copy(packet.begin(), packet.end(), buf.begin());
					g_dimensionstoypad.SendCommand(buf);
				}
			}

			uint32_t IdentifyFigure(const toypad_remote::Tag& tag) override
			{
				return g_dimensionstoypad.IdentifyFigure(tag);
			}

			std::string CatalogName(uint32_t id) override
			{
				const std::string name = g_dimensionstoypad.FindFigure(id);
				if (name.empty() || name == "unknown")
					return {};
				return name;
			}

			std::string FiguresDir() override
			{
				const fs::path dir = ActiveSettings::GetUserDataPath("dimensions");
				std::error_code ec;
				fs::create_directories(dir, ec);
				return _pathToUtf8(dir);
			}

			void Log(int level, const std::string& message) override
			{
				cemuLog_log(LogType::Force, "[{}] {}", level, message);
			}
		};

		CemuHost s_toypadHost; // nao chamar s_host: e um macro do winsock2.h

		// Config no formato antigo (<user data>/toypad_remote.txt: port=, pin=).
		void LoadConfig(toypad_remote::Config& cfg)
		{
			const fs::path cfgPath = ActiveSettings::GetUserDataPath("toypad_remote.txt");
			std::ifstream in(cfgPath);
			std::string line;
			while (std::getline(in, line))
			{
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				if (line.empty() || line[0] == '#')
					continue;
				const size_t eq = line.find('=');
				if (eq == std::string::npos)
					continue;
				const std::string key = line.substr(0, eq);
				const std::string value = line.substr(eq + 1);
				if (key == "port")
					cfg.port = std::clamp(std::atoi(value.c_str()), 1, 65535);
				else if (key == "pin")
					cfg.pin = value;
			}
		}
	} // namespace

	void Start()
	{
		if (s_started)
			return;
		s_started = true;

		toypad_remote::Config cfg;
		LoadConfig(cfg);
		std::string err;
		if (!toypad_remote::Core::Instance().Start(&s_toypadHost, cfg, err))
			cemuLog_log(LogType::Force, "ToypadRemote: {}", err);
	}

	void OnToypadCommand(std::span<const uint8, 32> buf)
	{
		if (!s_started)
			return;
		toypad_remote::Packet packet{};
		std::copy(buf.begin(), buf.end(), packet.begin());
		toypad_remote::Core::Instance().OnGameCommand(packet);
	}

	std::string GetUrl()
	{
		return s_started ? toypad_remote::Core::Instance().Url() : std::string();
	}

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
		(void)BTN_A, (void)BTN_B, (void)BTN_X, (void)BTN_Y, (void)BTN_L, (void)BTN_R;
		(void)BTN_UP, (void)BTN_DOWN, (void)BTN_LEFT, (void)BTN_RIGHT;

		// so contam botoes carregados com o Home ja em baixo
		const uint32 pressed = (modifierHeld && s_prevModifier) ? (hold & ~s_prevHold) : 0;
		s_prevHold = hold;
		s_prevModifier = modifierHeld;
		if (!modifierHeld)
			return false;
		(void)pressed; // F6: os atalhos passam a usar Core::SetSelected + os endpoints do telemovel
		// Enquanto o modificador esta em baixo os botoes nao chegam ao jogo (comportamento mantido).
		return true;
	}
} // namespace nsyshid::ToypadRemote
