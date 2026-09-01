# Glossary

The words the comments in this plugin use for its own parts. Every entry here
was checked against the code, not guessed at.

## Surfaces the player sees

**board** — one inventory grid. The player has three (ITEMS, QUEST, KEYS) and
they are all the same shape, set by `!basegrid` in `GridInventory_ui.ini`. Every
open bag draws a board of its own as well. Width and height are read through
`Grid::BaseCols()` and `Grid::BaseRows()`, never as a constant.

**cell** — one 1×1 square of a board. The unit the board is measured in.

**tile** — one item sitting on a board, occupying a rectangle of cells. A tile
knows its item, how many of it there are, where it sits and which way round it
is turned.

**pack** — the player's own inventory: their boards plus every bag they own.
"The pack is full" means no board of the player's has room.

**doll** — the equipment panel: the 3×4 grid of worn-gear slots (head, necklace,
chest, hands, `ringR`, `ringL`, weapon, shield, boots, ammo…) with a vertical
strip of a circlet slot and four accessory slots beside it. The standard RPG
"paper doll". Drawn by `Equip::DrawDoll`; slot names are the string ids in
`kStrip` and `kDoll` in `Equip.cpp`.

**partner** — whatever is on the other side of a loot or trade window: a chest,
a corpse, a merchant, or a follower. Its board is the **shelf**.

**shelf** — the partner's board. A follower's shelf is bounded (they can only
carry so much); a chest's is not.

## Items, and telling copies apart

Three different things get called "the item", and the comments are careful about
which one they mean.

**form** — the record itself (`TESForm` / `TESBoundObject`). Every iron dagger in
the game shares one form. A form knows nothing about temper, enchantment or
ownership.

**unit** — one physical copy of a form. Two units of one form can differ: one
tempered, one not; one stolen, one bought.

**list** — an `ExtraDataList`, the engine's per-unit data. What makes a unit
different from its siblings lives here. A unit with nothing unusual about it
carries no list at all, and is called **plain**.

**pool** — the set of units that cannot be told apart, and so are
interchangeable. A tile draws a pool, not a unit.

A unit is named by one of two things, and the difference matters constantly:

**uid** — the engine's `ExtraUniqueID`. When a unit has one it is the sole
member of its own pool. Not every save hands them out.

**sig** — a *content signature*: an FNV-1a hash of the facts on the unit's list
that actually distinguish it (foreign ownership, enchantment, temper, charge,
poison, a player-typed name). Computed by `InstanceSig` in `Grid.cpp`. **sig 0
means plain** — nothing distinguishes this unit — and never "unknown".

So "name a unit by uid, else by sig, and never by its position in a list" is the
rule the codebase keeps returning to. A list *position* goes stale: the engine
reorders lists behind us, and `AddExtraList` prepends, so an index recorded when
a tile was born can point at a different unit later. The resolvers are
`ExtraForPool` (by identity, skips worn lists), `ExtraForTile` (by position) and
`ExtraForUnit` (identity first, position as a last resort).

**worn list** — the list of a unit currently on the body. It always exists,
because being worn is itself something the engine records (`ExtraWorn`).

## Stacks

**stack cap** — how many of a thing fit in one tile, from the item's definition
(`Grid::StackCap`).

**capful** — one full stack. "Drawn as a capful" means shown at the cap, whatever
the real total is.

**mint** — to create a new tile. Contrasted with **filling** an existing partial
tile. When a stack arrives, the question is always whether it tops up a tile
that is already there or mints one of its own.

**aim / hint** — the cell the player actually dropped on. An aimed drop is
supposed to land where it was aimed; an unaimed arrival (loot, a purchase, a
reward) fills partial tiles first.

## Machinery

**rebuild** — a full re-read of the inventory that re-places every tile. Correct
but expensive.

**fast path / partial** — the optimistic update that adjusts the board in place
without a rebuild. It only handles states it can prove, and declines everything
else with a reason so the rebuild can do the whole job.

**reconcile** — the pass that mints cells for units the engine has confirmed
moved.

**anchor** — a costume system placeholder: a 0/0/0 armour record with no model,
equipped purely so a bare slot has an appearance list to lend. Never the
player's property, and hidden from the board, the doll and every transfer.

**carrier** — *retired in 1.6.0.* An invisible item that used to be worn in place
of a second ring, so the ring's enchantment could be copied onto it. Replaced by
taking the `kRing` slot bit off the ring already worn, which lets the engine wear
both rings for real. Comments mentioning a carrier are describing history.

**spot key** — a stable name for one place on a shelf, so a container remembers
where its contents sat between visits.
