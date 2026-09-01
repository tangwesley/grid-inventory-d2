#pragma once

#include "ui/Grid.h"

#include <string>

namespace FUI::Equip
{
    // B-5 (PLAN_B section 2-D): the equipment doll. A vertical strip (circlet
    // plus four accessory cells) beside a 3x4 grid of gear slots. Worn items
    // draw their cached icons, right-clicking a slot unequips, and a carried
    // item can be dropped onto a slot to equip it (C6/D4).

    // Equip an item, subject to the vanilla type gate (WEAP / ARMO / AMMO /
    // LIGHT / ALCH / SCRL). Passing a_slotId == "shieldL" routes one-handers
    // and staves to the LEFT hand (BGSEquipSlot 0x13F43). Returns true when the
    // gate accepted the item.
    //
    // GI1/D4: a_uid and a_xlIdx say WHICH copy to equip. Every
    // ActorEquipManager call used to pass a null ExtraDataList, which lets the
    // ENGINE choose the sub-stack -- so equipping the enchanted one of three
    // identical swords was a coin flip. Passing nullptr/0/-1 keeps the old
    // "let the engine pick" behaviour, for callers that genuinely have no
    // particular unit in hand.
    //
    // a_tileCount is how many the tile being acted on holds. Only AMMO uses it
    // (see EquipCountFor); every other kind of item is equipped one at a time
    // regardless of what the tile holds.
    bool EquipItem(RE::TESBoundObject* a_obj, const std::string& a_slotId,
                   std::uint16_t a_uid, int a_xlIdx, std::uint16_t a_sig,
                   const std::string& a_srcKey = {}, int a_tileCount = 1);

    // A quiver is not equipped one arrow at a time. Vanilla puts the whole lot
    // on your back, and here the TILE owns the count (G4) -- so equipping a tile
    // equips what that tile holds, and 200 arrows spread across two tiles means
    // clicking one leaves the other in the pack.
    //
    // Taking a single arrow off a stack was the old behaviour, and it was wrong
    // twice over: it did not match vanilla, and the leftover split lists are
    // what made repeated clicks eat the stack.
    //
    // Everything else stays at one: a tile of ten potions is ten drinks.
    [[nodiscard]] int EquipCountFor(RE::TESBoundObject* a_obj, int a_tileCount);

    // TWO AMMO RULES USED TO LIVE HERE AND BOTH ARE GONE (1.6.1).
    //
    //   AmmoMergeRoom      how much room is left on the player's back
    //   NormaliseWornAmmo  take the over-cap surplus back off, on every delta
    //
    // Both existed to keep the engine's worn count at a number the board could
    // draw. Nothing reads that number now: the doll shows min(total, cap) and
    // the board shows the remainder, so "40 worn" and "240 worn" produce the
    // same picture. The first rule was deciding something invisible; the second
    // was a tug of war that the engine undid on the very next delta.
    //
    // Removing them also removed an unequip of the entire quiver on every
    // "replace" click, and a 3D model reset on every arrow retrieved while at
    // the cap. Full story and measurements are in PLAN_AMMO_TOTALS.md.

    // USING an item is not WEARING it, and only the second has a type gate that
    // makes sense. EquipItem's whitelist answers "will a doll slot take this?".
    // Asking it "does clicking this do anything?" made us answer on behalf of
    // the engine, which we are not entitled to do: a mod can build a click-me
    // item out of whatever record type suits it (AddItemMenu's cube is one),
    // its script reacts to the equip, and no whitelist of ours can know about
    // it. The vanilla inventory does not pre-judge either -- it hands the item
    // over and lets the engine decide. So does this: same queue, same tick, no
    // slot, and the engine decides what the click meant.
    bool UseItem(RE::TESBoundObject* a_obj, std::uint16_t a_uid, int a_xlIdx,
                 std::uint16_t a_sig, const std::string& a_srcKey = {},
                 int a_tileCount = 1);

    // Which ammo is equipped, without going through a vtable.
    // Actor::GetCurrentAmmo is virtual, and the two CommonLib lines disagree
    // about its slot: 0x9E in the tree we used to build against, 0x9F in the
    // one that reads 1.7.99's address library. An off-by-one there does not
    // fail loudly -- it calls the neighbouring function and returns whatever
    // that gives back, which is how equipped arrows silently vanished from the
    // doll. Equipped ammo is simply the ammo the engine has marked worn, and
    // finding that needs no vtable at all.
    [[nodiscard]] RE::TESAmmo* EquippedAmmo(RE::Actor* a_actor);

    // Take a worn item off. This uses the same queue as everything else, so it
    // is safe to call from the render pass. The doll has always offered this on
    // right-click, but only through its own slot widget; surfaces with no doll
    // -- the quick wheel -- need it as a plain call.
    //
    // a_hand: 0 = none, 1 = right, 2 = left. It matters when both hands wear
    // identical units; 0 lets the engine pick, which is correct for armour.
    bool UnequipItem(RE::TESBoundObject* a_obj, std::uint16_t a_uid,
                     std::uint16_t a_sig, int a_hand = 0, int a_count = 1);

    // Ring session: a cancelled carry goes back to the slot it was lifted from
    // (the origin rule). This queues through the same pending pipeline as every
    // other equip, so the conflict pass and the srcList resolve both apply.
    // a_hand == 2 sends a one-hander back to the left hand.
    void RequestWear(RE::TESBoundObject* a_obj, std::uint16_t a_uid,
                     std::uint16_t a_sig, int a_hand, int a_count);

    // Does using this take the unit off the board -- worn, drunk, eaten,
    // learnt? The board bookkeeping (vacate the cell, forget the tile, hint the
    // drain) has to run for those and must NOT run for a scripted item that is
    // only being poked: forgetting the cell of something that never left makes
    // it jump to the first free slot on every click.
    [[nodiscard]] bool IsWearOrConsume(RE::TESBoundObject* a_obj);

    // The object and sub-stack worn in a doll slot ("weapon", "shieldL",
    // "head"...). Needed by anything that has to act on the WORN copy rather
    // than on the form.
    [[nodiscard]] RE::TESBoundObject* WornObjectAt(const std::string& a_slotId);
    [[nodiscard]] RE::ExtraDataList*  WornExtraAt(const std::string& a_slotId);

    // How many units the slot is wearing. This is NOT derivable from
    // WornExtraAt: ammo can be worn with no ExtraDataList at all, and reading
    // GetCount() off a null list quietly answered "1" for a hundred-arrow
    // quiver. The slot already knows the number, because CollectEquipment
    // counts it -- so ask the slot.
    [[nodiscard]] int WornCountAt(const std::string& a_slotId);

    // Equip and unequip requests queue here and run in the game-update hook
    // (UIRoot::Tick). Calling ActorEquipManager from inside the UI render pass
    // defers the 3D refresh until the menu closes, because the game is paused.
    void ProcessPending();

    // Draw the doll panel at the current cursor position, inside the main
    // window.
    void Draw();

    // L2: the loadout buy/delete confirmation windows. Call these at TOP LEVEL,
    // like the settings window, and NOT inside the equip panel's child window.
    void DrawLoadoutWindows();

    // GI53: drop popup state (the buy and delete asks) when the menu closes. A
    // confirmation left open used to reappear the next time the menu opened.
    void OnMenuClosed();

    // True while either loadout popup is up. This contributes one layer to the
    // close-order stack; CloseTopPopup keeps its own buy-then-delete ordering
    // internally.
    [[nodiscard]] bool IsPopupOpen();
    bool CloseTopPopup();   // I/ESC layering: close an open buy/delete popup

    // v9 spec: every slot is 2x2 inventory cells; weapon, body and shield are
    // 2x4 (plus 5px).
    //
    // kDollTrim: the doll is measured in cells like everything else, but it --
    // not the grid -- decides how tall the left column is, and the stats panel
    // below it is text, so it does not shrink along with the cells. Taking 5%
    // off the slots (and halving the gaps) buys back the room the stats panel
    // needs at small cell sizes, without changing a single font size.
    inline constexpr float kDollTrim = 0.95f;
    [[nodiscard]] inline float SlotPx() { return Grid::CellPx() * 2.0f * kDollTrim; }
    [[nodiscard]] inline float GapPx() { return 2.5f * Theme::Scale(); }

    // A tall slot must equal exactly two short slots plus the gap between them.
    // That identity is what keeps the weapon/body column level with the
    // accessory column beside it. The old literal "+5" happened to equal the
    // gap, so halving the gap silently broke the identity and the two columns
    // drifted apart by exactly the difference. State it as the identity rather
    // than as a number.
    [[nodiscard]] inline float TallPx() { return 2.0f * SlotPx() + GapPx(); }

    // The vertical strip (circlet + 4 accessories) plus the doll's 3 columns.
    [[nodiscard]] inline float PanelW() { return 4 * SlotPx() + 3 * GapPx(); }

    // The tabs row plus the doll's rows: [2 units, 2x4, 2 units, 2 units].
    [[nodiscard]] inline float PanelH()
    {
        return 30.0f * Theme::Scale() + 3 * SlotPx() + TallPx() + 3 * GapPx();
    }

    // GI77: how far the first slot row sits below the top of the column -- in
    // other words, how tall the loadout tab strip actually turned out to be.
    // This is MEASURED during the last Draw() rather than assumed, because the
    // 30*Scale above is a reservation that rounds generously, and the item grid
    // has to line up with the real slot edge rather than with a budget. Returns
    // 0 before the first draw.
    [[nodiscard]] float SlotsTopOffset();

    // The same row, in SCREEN coordinates. This is not derivable from the
    // offset above: Draw() runs inside the "fab_left" CHILD window, so that
    // number is relative to the child, and adding it to the MAIN window's
    // origin lands one child's worth too high. (It did -- the drawer's first
    // row sat above the doll's.) Anything drawing outside both windows wants
    // this one.
    [[nodiscard]] float SlotsTopScreen();

    // ---- accessory drawer (PLAN_ACCESSORY_DRAWER.md) ----------------------
    //
    // The doll holds four accessory cells (acc1..acc3 plus accM -- the strip's
    // fifth cell is the drawer's button), and any further worn accessory used
    // to be DROPPED: silently, with nothing on screen and nothing in the log.
    // Outfit mods routinely equip eight to fifteen of them -- scarves, bags,
    // belts, glasses, cloaks and piercings all land on biped slots 44..60,
    // which SlotForArmor has no name for.
    //
    // The drawer holds the rest of that list, in a panel docked to the main
    // window's left edge. The strip is effectively column 0 and the drawer
    // continues it, so between them nothing worn is invisible.

    // The lowest biped slot (30..61) this armour occupies, or a large number
    // when it claims none.
    //
    // This is what decides where an accessory sits -- not the order it was
    // equipped in. Assignment used to be `accNext++`, so taking one item off
    // and putting it back rearranged all the others. A slot number cannot
    // drift: wear the same outfit and you get the same cells.
    [[nodiscard]] int PrimarySlot(RE::TESBoundObject* a_obj);

    // How many worn accessories did not fit into the doll's five cells. May be
    // 0 -- the tab shows regardless, because a drawer that only appears once
    // you are already overflowing is a drawer nobody knows exists.
    [[nodiscard]] int DrawerCount();

    [[nodiscard]] bool DrawerOpen();
    void               SetDrawerOpen(bool a_open);

    // Not available during loot or barter. The doll itself is hidden there
    // (the partner window takes that side of the screen), so a drawer hanging
    // off it would be a panel belonging to something the player cannot see.
    [[nodiscard]] bool DrawerAvailable();

    // Draws BOTH the tab and the panel, from outside the main window. UIRoot
    // calls this at top level, alongside the shelf-bag windows.
    //
    // The tab sits in the main window's own margin, level with the first slot
    // row, and stays there. It used to ride the panel's outer edge, so the
    // control that opens the drawer travelled with the drawer and had to be
    // hunted down again after every slide. It still cannot be drawn BY the main
    // window, because a widget clips to the window that declares it and the
    // panel it toggles lies entirely outside that rectangle -- so one function
    // owns both halves, and a slide can never leave them disagreeing.
    //
    // The position is derived rather than remembered: the panel's inner edge is
    // the main window's left edge, and its first CELL (not its frame) sits on
    // the doll's first slot row. So moving the window moves the drawer with it,
    // and the two grids read as a single column.
    void DrawDrawer(const ImVec2& a_mainPos, const ImVec2& a_mainSize,
                    float a_rowTopScreen);

    // What the TOOLTIP says an item is worn on. It has to agree with the doll:
    // SlotAccepts already files every biped slot this UI has no name for as an
    // ACCESSORY, so an unknown slot reads "Accessory (47)" rather than having a
    // name invented for it.
    //
    // The number is the SLOT (30..61), not the bit index. The slot number is
    // what mod pages and conflict guides quote, and so the one a player can
    // actually act on.
    //
    // Returns an empty string when the item is not worn at all.
    [[nodiscard]] std::string SlotLabel(RE::TESBoundObject* a_obj);
}
