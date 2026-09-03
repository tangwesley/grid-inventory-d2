#pragma once

#include <imgui.h>

namespace FUI
{
    // Ported from ModExplorerMenu (Modex) by patchulidev — ModexGUIMenu.
    // https://github.com/patchulidev/ModExplorerMenu (GPL-3.0 with Modding Exception)
    //
    // The kCustomRendering menu whose PostDisplay is the game's UI render
    // pass — the only stage where Inventory3DManager::Render() actually
    // draws (MODEX_TRANSITION_PLAN §2 root cause). Also relays Scaleform
    // input events into ImGui.
    class GridInventoryMenu : public RE::IMenu
    {
    public:
        static constexpr std::string_view MENU_NAME = "GridInventoryMenu";

        static void RegisterMenu();
        static void FlushInputState();

        // F1: the titlebar ✕ plays MenuClose itself before UIRoot::Close();
        // this suppresses OnHide's fallback so the sound doesn't double up.
        static void MarkCloseSfxPlayed();

        // The "!nopause" setting in GridInventory_ui.ini. On, the grid drops
        // kPausesGame and opens onto a live world: the game keeps running
        // while the player browses. Off (the code default, and what a file
        // without the line means) the grid pauses like the vanilla menu.
        //
        // It began as a measurement switch, and the measurement is what made
        // it a setting: the class comment above names the render STAGE as the
        // requirement for the engine's item 3D preview, while Modex's config
        // note, carried over with the port, named the PAUSE as well. Compared
        // in pixels, paused and live render the same preview byte for byte,
        // so the pause was never a requirement.
        //
        // Three things follow the flag off, and every one has to stay in
        // step with it: AdvanceMovie's Tick is conditional (the update hook
        // runs unpaused, so it would fire twice a frame), main.cpp masks the
        // gameplay input layer so the player cannot swing or shout while
        // navigating the board, and anything expensive on the open or close
        // frame is a visible stall rather than a hidden one. Equip.cpp's
        // same-slot conflict resolution was written for a paused engine and
        // still runs by hand on a live one.
        [[nodiscard]] static bool NoPause();
        static void                SetNoPause(bool a_on);

        void PostDisplay() override;
        void AdvanceMovie(float a_interval, std::uint32_t a_currentTime) override;
        RE::UI_MESSAGE_RESULTS ProcessMessage(RE::UIMessage& a_message) override;

    private:
        void OnShow();
        void OnHide();

        static RE::IMenu* Creator();

        static void ProcessScaleformEvent(const RE::BSUIScaleformData* a_data);
        static void OnMouseEvent(RE::GFxEvent* a_event, bool a_down);
        static void OnMouseWheelEvent(RE::GFxEvent* a_event);
        static void OnKeyEvent(RE::GFxEvent* a_event, bool a_down);
        static void OnCharEvent(RE::GFxEvent* a_event);
        static void ForceCursor();

        static ImGuiKey GFxKeyToImGuiKey(RE::GFxKey::Code a_keyCode);

        // This is the tail that VR's engine writes into, and the reason a VR
        // player crashed.
        //
        // RE::IMenu is 0x40 bytes on VR and 0x30 on SE/AE
        // (STATIC_ASSERT_SIZE(IMenu, 0x30, 0x30, 0x40, 0x30)), and a cross-VR
        // CommonLibSSE build compiles the 0x30 layout -- the VR-only fields sit
        // behind `#if defined(EXCLUSIVE_SKYRIM_VR)` and are simply absent. So
        // `new GridInventoryMenu()` asked the heap for 0x30 bytes while the VR
        // engine carried on writing unk30, unk34 and menuName into 0x30..0x3F:
        // sixteen bytes past the end of the allocation, straight into whatever
        // block happened to follow.
        //
        // The crash log that named it: `mov [rax], r8` with
        // rax = 0x6E654D79726F746E, which is "ntoryMen" little-endian — bytes
        // 8..15 of "GridInventoryMenu". A pointer field had been read out of a
        // string buffer, which is what a smashed neighbouring block looks like.
        //
        // Declared in VR's own order so the offsets land exactly where the
        // engine expects (0x30 / 0x34 / 0x38), rather than as opaque padding:
        // the BSFixedString then also releases its ref when we are destroyed.
        // SE/AE never touch these — the cost there is sixteen bytes per menu.
        REX::EnumSet<RE::UI_MENU_Unk09, std::uint32_t> _vrUnk30{ RE::UI_MENU_Unk09::kNone };
        std::byte                                     _vrUnk34{ std::byte{ 1 } };
        RE::BSFixedString                             _vrMenuName{};
    };

    static_assert(sizeof(GridInventoryMenu) >= 0x40,
        "VR's IMenu is 0x40 -- the engine writes past a 0x30 allocation");
}
