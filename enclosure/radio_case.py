#!/usr/bin/env python3
"""Parametric retro tabletop-radio enclosure for the Hosyond ESP32-S3 4.0".

Two printed parts, four self-tapping screws, nothing else. The board is NOT
screwed to anything: it drops into a printed pocket that locates it in X and Y,
and the back cover's pads hold it forward against the inside of the front face.
That removes the need to know where the mounting holes are, and it removes the
usual boss-height problem - a self-tapper needs ~5 mm of thread, which would
otherwise push the board 5 mm back and turn the window into a tunnel.

    front.stl  the body: tilted face, screen window, slotted grille, speaker
               ring, four corner bosses, USB slot
    back.stl   flat cover: vents, four screw holes, four board pads

EVERY BOARD DIMENSION BELOW IS A PLACEHOLDER until measured. The vendor was
wrong about the panel size, the driver IC and the amp polarity, so nothing here
is taken on trust either. Measure, edit the block, re-run.

Copyright (c) 2026 Rich Sadowsky. MIT licensed - see LICENSE.
Written with Claude Code (Claude Opus 5).
"""

import math
import os

import numpy as np
import trimesh
from shapely.geometry import Polygon

# ---------------------------------------------------------------------------
#  MEASURE THESE  (mm)
# ---------------------------------------------------------------------------
BOARD_W      = 112.6   # PCB outline, long edge   MEASURED
BOARD_H      = 60.98   # PCB outline, short edge  MEASURED
BOARD_T      = 1.6     # PCB thickness
BACK_CLEAR   = 13.0    # tallest thing on the BACK, from the PCB surface

ACTIVE_W     = 84.6    # panel active area (4.0" diagonal at 3:2)
ACTIVE_H     = 56.4
ACTIVE_DX    = 0.0     # active-area centre offset from PCB centre, +X = right
ACTIVE_DY    = 0.0     # ... +Y = up

# Speaker: RECTANGULAR with rounded corners, adhesive pad on the back, no
# mounting holes. Not the round driver the first version assumed.
SPK_W        = 40.62   # long edge        MEASURED
SPK_H        = 28.06   # short edge       MEASURED
SPK_DEPTH    = 9.71    # body depth       MEASURED
SPK_CORNER   = 3.0     # corner radius, eyeballed - only affects pocket fit
SPK_VERTICAL = True    # long edge runs up the face, matching the slots

USB_EDGE     = "right"  # which PCB edge the USB-C port is on: left/right/bottom
USB_W, USB_H = 11.2, 6.2   # measured
USB_POS      = 0.0     # offset along that edge from centre

# ---------------------------------------------------------------------------
#  Case style - safe to play with
# ---------------------------------------------------------------------------
WALL      = 2.4    # 6 perimeters at 0.4 mm; stiff enough not to drum
TILT      = 15.0   # front face lean-back, degrees
MARGIN_H  = 9.0    # bezel left/right of the board on the front face
MARGIN_V  = 13.0   # ...and above/below. Deliberately bigger than MARGIN_H.
                   # Equal margins on a 108 mm board gave a 2.2:1 letterbox
                   # that read as a car stereo. Tabletop radios sit nearer
                   # 1.7:1, and the vertical bezel is what buys it.
PLINTH_H  = 11.0   # vertical front below the tilted face - the foot every
                   # mantel radio has, and what stops a 40 mm-deep case from
                   # looking like it is about to topple forwards.
GAP       = 6.0    # between board area and grille
CLEAR     = 0.5    # fit clearance around the board in its pocket
POCKET_H  = 3.2    # pocket rim height - twice PCB thickness, so it cannot ride up
AIR       = 2.5    # air behind the board's tallest component

SCREW_PILOT = 2.1  # M2.5 self-tapper into plastic
SCREW_HEAD  = 5.2
BOSS_DIA    = 6.4

# Grille: VERTICAL slots, rounded ends.
#
# Vertical is a print-quality choice, not a style one. Printed plinth-down, a
# horizontal slot needs the wall above it to bridge the gap, so every slot gets
# a rough underside. Vertical slots run along the build direction: the fins
# between them are continuous and every layer is supported by the one below, so
# they come out crisp with no supports.
#
# The grille panel is deliberately wider than the speaker. Sized to the speaker
# alone it only fitted five slots and looked sparse; the extra width buys eight
# without moving the speaker.
GRILLE_SLOT_W  = 2.2    # slot width
GRILLE_GAP     = 2.4    # fin between slots
GRILLE_MARGIN  = 6.0    # border inside the grille panel
GRILLE_EXTRA   = 24.0   # panel width beyond the speaker diameter

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "stl")

# ---------------------------------------------------------------------------
#  Derived
# ---------------------------------------------------------------------------
t = math.radians(TILT)
SPK_ACROSS = SPK_H if SPK_VERTICAL else SPK_W   # horizontal extent on the face
SPK_ALONG  = SPK_W if SPK_VERTICAL else SPK_H   # vertical extent on the face

GRILLE_W = SPK_ACROSS + GRILLE_EXTRA
GRILLE_LEN = SPK_ALONG + 16.0   # slot length, centred on the speaker

SPK_FIT    = 0.25   # pocket undersize for a friction fit
SPK_FRAME  = 2.6    # pocket wall thickness
SPK_LIP    = 1.2    # air gap between diaphragm and the grille wall
SPK_RELIEF = 5.0    # how far the lip opening is inset from the speaker edge

# The front face, measured along its own slope.
FACE_W   = 2 * MARGIN_H + BOARD_W + GAP + GRILLE_W
FACE_LEN = BOARD_H + 2 * MARGIN_V

LEAN   = FACE_LEN * math.sin(t)          # how far the top sets back
HEIGHT = PLINTH_H + FACE_LEN * math.cos(t)
INNER  = BOARD_T + BACK_CLEAR + AIR      # clear depth needed behind the face
DEPTH  = LEAN + INNER + WALL             # total footprint depth

# Bosses must clear the wall, not straddle it. At BOSS_DIA/2 + 1.0 the boss sat
# half inside the side wall and - worse - the matching hole in the back cover
# came out exactly TANGENT to the cover's edge, which is a zero-thickness sliver
# and leaves the mesh non-manifold. The watertight check caught it; a slicer
# might have silently "repaired" it into something wrong.
BOSS_INSET = WALL + BOSS_DIA / 2 + 1.0


def face_transform():
    """World placement of the tilted front face.

    Face coordinates are (u, v, w): u across the width, v up the slope from the
    bottom front edge, w outward along the face normal. Everything cut into the
    face is built in these coordinates and then mapped once, which keeps the
    trigonometry in a single place instead of smeared through every feature.
    """
    u = np.array([1.0, 0.0, 0.0])
    v = np.array([0.0, math.sin(t), math.cos(t)])
    w = np.array([0.0, -math.cos(t), math.sin(t)])
    m = np.eye(4)
    m[:3, 0], m[:3, 1], m[:3, 2] = u, v, w
    m[:3, 3] = np.array([0.0, 0.0, PLINTH_H])  # face starts atop the plinth
    return m


def cutter(du, dv, dw, cu, cv, cw):
    """A box in face coordinates, returned in world space."""
    b = trimesh.creation.box(extents=[du, dv, dw])
    shift = np.eye(4)
    shift[:3, 3] = [cu, cv, cw]
    b.apply_transform(face_transform() @ shift)
    return b


def side_profile(inset=0.0):
    """The wedge, as a 2D polygon in (depth, height). Extruded along the width.

    Front edge leans back as it rises, so the face is a 75-degree wall - steep
    enough to print with no support at all, which matters when the part is going
    to a print service rather than a machine you can babysit.
    """
    lift = inset / math.cos(t)
    return Polygon([
        (inset, inset),
        (DEPTH - inset, inset),
        (DEPTH - inset, HEIGHT - inset),
        (LEAN + lift * math.sin(t), HEIGHT - inset),
        (inset, PLINTH_H + lift),
    ])


def body():
    outer = trimesh.creation.extrude_polygon(side_profile(), FACE_W)
    # extrude_polygon builds in XY and extrudes +Z; rotate so the polygon's
    # (depth, height) becomes world (Y, Z) and the extrusion becomes world X.
    r = trimesh.transformations.rotation_matrix(math.radians(90), [0, 1, 0])
    r2 = trimesh.transformations.rotation_matrix(math.radians(90), [0, 0, 1])
    outer.apply_transform(r2 @ r)
    outer.apply_transform(trimesh.transformations.translation_matrix(
        [0, 0, 0]))
    return outer


def main():
    os.makedirs(OUT, exist_ok=True)

    # --- outer wedge, hollowed -------------------------------------------
    prof_o = side_profile()
    prof_i = side_profile(WALL)
    solid = trimesh.creation.extrude_polygon(prof_o, FACE_W - 0.0)
    void = trimesh.creation.extrude_polygon(prof_i, FACE_W - 2 * WALL)

    # Orient: polygon XY -> world YZ, extrusion Z -> world X.
    m = np.array([[0, 0, 1, 0],
                  [1, 0, 0, 0],
                  [0, 1, 0, 0],
                  [0, 0, 0, 1]], dtype=float)
    solid.apply_transform(m)
    void.apply_transform(m)
    void.apply_transform(trimesh.transformations.translation_matrix([WALL, 0, 0]))

    shell = solid.difference(void, engine="manifold")

    # Open the back so the cover can go on, and so the void is reachable.
    back_open = trimesh.creation.box(extents=[FACE_W - 2 * WALL, WALL * 3,
                                              HEIGHT - 2 * WALL])
    back_open.apply_transform(trimesh.transformations.translation_matrix(
        [FACE_W / 2, DEPTH - WALL / 2, HEIGHT / 2]))
    shell = shell.difference(back_open, engine="manifold")

    # --- screen window ----------------------------------------------------
    u0 = MARGIN_H + BOARD_W / 2 + ACTIVE_DX
    v0 = MARGIN_V + BOARD_H / 2 + ACTIVE_DY
    shell = shell.difference(
        cutter(ACTIVE_W + 1.0, ACTIVE_H + 1.0, 60.0, u0, v0, -28.0),
        engine="manifold")

    # --- board pocket -----------------------------------------------------
    # A rim standing off the inside of the front face. Locates the board in u
    # and v; the back cover's pads hold it in w. No screws through the board.
    rim_o = cutter(BOARD_W + 2 * CLEAR + 2 * WALL, BOARD_H + 2 * CLEAR + 2 * WALL,
                   POCKET_H, MARGIN_H + BOARD_W / 2, MARGIN_V + BOARD_H / 2,
                   -POCKET_H / 2 - WALL + 0.01)
    rim_i = cutter(BOARD_W + 2 * CLEAR, BOARD_H + 2 * CLEAR, POCKET_H + 2,
                   MARGIN_H + BOARD_W / 2, MARGIN_V + BOARD_H / 2,
                   -POCKET_H / 2 - WALL)
    shell = shell.union(rim_o.difference(rim_i, engine="manifold"),
                        engine="manifold")

    # --- grille -----------------------------------------------------------
    gu = MARGIN_H + BOARD_W + GAP + GRILLE_W / 2
    gv = FACE_LEN / 2                      # centred on the speaker
    pitch = GRILLE_SLOT_W + GRILLE_GAP
    usable = GRILLE_W - 2 * GRILLE_MARGIN
    n_slot = int((usable + GRILLE_GAP) // pitch)
    span = n_slot * pitch - GRILLE_GAP
    u_start = gu - span / 2 + GRILLE_SLOT_W / 2

    def slot_cutter(cu):
        """One slot: a box with a half-round cap at each end.

        Rounded ends are worth the extra two cylinders - a square-cornered slot
        in a 2.4 mm wall puts a stress riser at each corner, and the corners are
        where a thin printed fin lets go first.
        """
        straight = GRILLE_LEN - GRILLE_SLOT_W
        parts = [cutter(GRILLE_SLOT_W, straight, WALL * 4, cu, gv, -WALL)]
        for dv in (-straight / 2, straight / 2):
            c = trimesh.creation.cylinder(radius=GRILLE_SLOT_W / 2, height=WALL * 4)
            place = np.eye(4)
            place[:3, 3] = [cu, gv + dv, -WALL]
            c.apply_transform(face_transform() @ place)
            parts.append(c)
        out = parts[0]
        for q in parts[1:]:
            out = out.union(q, engine="manifold")
        return out

    for i in range(n_slot):
        shell = shell.difference(slot_cutter(u_start + i * pitch),
                                 engine="manifold")

    # --- speaker pocket ---------------------------------------------------
    #
    # The speaker is a rounded rectangle with an adhesive pad on the back and no
    # mounting holes, so there is nothing to screw and nothing round to press
    # into a ring. It goes into a pocket instead: friction on four sides, and a
    # shoulder at the front that holds the diaphragm SPK_LIP clear of the grille
    # wall. Resting a cone directly against flat plastic muffles it.
    #
    # The adhesive is then a bonus rather than the mechanism - peel it and it
    # grabs the pocket wall - which matters because sticking a speaker to the
    # inside of a wall it has to fire through is not a thing that works.
    def rrect(w, h, r):
        from shapely.geometry import box as _box
        r = min(r, w / 2 - 0.01, h / 2 - 0.01)
        return _box(-w / 2 + r, -h / 2 + r, w / 2 - r, h / 2 - r).buffer(r)

    def face_solid(poly, depth, w_front):
        """Extrude a face-space polygon inward from w_front."""
        m = trimesh.creation.extrude_polygon(poly, depth)
        place = np.eye(4)
        place[:3, 3] = [gu, gv, w_front - depth]
        return m.apply_transform(face_transform() @ place) or m

    pw, ph = (SPK_ACROSS, SPK_ALONG)
    pocket_d = SPK_DEPTH + 2.0

    frame = face_solid(rrect(pw + 2 * SPK_FRAME, ph + 2 * SPK_FRAME,
                             SPK_CORNER + SPK_FRAME), pocket_d, -WALL)
    cavity = face_solid(rrect(pw - SPK_FIT, ph - SPK_FIT, SPK_CORNER),
                        pocket_d + 2.0, -WALL - SPK_LIP)
    relief = face_solid(rrect(pw - 2 * SPK_RELIEF, ph - 2 * SPK_RELIEF,
                              SPK_CORNER), SPK_LIP + WALL * 3, -WALL + WALL * 2)

    shell = shell.union(frame, engine="manifold")
    shell = shell.difference(cavity, engine="manifold")
    shell = shell.difference(relief, engine="manifold")

    # --- corner screw bosses ---------------------------------------------
    boss_len = INNER - 1.0
    for sx in (BOSS_INSET, FACE_W - BOSS_INSET):
        for sz in (BOSS_INSET, HEIGHT - BOSS_INSET):
            b = trimesh.creation.cylinder(radius=BOSS_DIA / 2, height=boss_len)
            b.apply_transform(trimesh.transformations.rotation_matrix(
                math.radians(90), [1, 0, 0]))
            b.apply_transform(trimesh.transformations.translation_matrix(
                [sx, DEPTH - WALL - boss_len / 2, sz]))
            p = trimesh.creation.cylinder(radius=SCREW_PILOT / 2, height=boss_len + 4)
            p.apply_transform(trimesh.transformations.rotation_matrix(
                math.radians(90), [1, 0, 0]))
            p.apply_transform(trimesh.transformations.translation_matrix(
                [sx, DEPTH - WALL - boss_len / 2, sz]))
            shell = shell.union(b, engine="manifold").difference(p, engine="manifold")

    # --- USB slot ---------------------------------------------------------
    if USB_EDGE in ("left", "right"):
        x = 0.0 if USB_EDGE == "left" else FACE_W
        u_slot = trimesh.creation.box(extents=[WALL * 4, USB_H + 2, USB_W + 2])
        u_slot.apply_transform(trimesh.transformations.translation_matrix(
            [x, LEAN / 2 + INNER / 2, HEIGHT / 2 + USB_POS]))
        shell = shell.difference(u_slot, engine="manifold")

    shell.export(os.path.join(OUT, "front.stl"))

    # --- back cover -------------------------------------------------------
    cov = trimesh.creation.box(extents=[FACE_W - 2 * WALL - 0.4,
                                        WALL, HEIGHT - 2 * WALL - 0.4])
    cov.apply_transform(trimesh.transformations.translation_matrix(
        [FACE_W / 2, DEPTH - WALL / 2, HEIGHT / 2]))
    for sx in (BOSS_INSET, FACE_W - BOSS_INSET):
        for sz in (BOSS_INSET, HEIGHT - BOSS_INSET):
            h = trimesh.creation.cylinder(radius=SCREW_HEAD / 2 - 1.0, height=WALL * 4)
            h.apply_transform(trimesh.transformations.rotation_matrix(
                math.radians(90), [1, 0, 0]))
            h.apply_transform(trimesh.transformations.translation_matrix(
                [sx, DEPTH - WALL / 2, sz]))
            cov = cov.difference(h, engine="manifold")
    # Vents. The ES8311 and the S3 both run warm in a sealed box.
    for i in range(-3, 4):
        vent = trimesh.creation.box(extents=[3.0, WALL * 4, HEIGHT * 0.45])
        vent.apply_transform(trimesh.transformations.translation_matrix(
            [FACE_W / 2 + i * 8.0, DEPTH - WALL / 2, HEIGHT / 2]))
        cov = cov.difference(vent, engine="manifold")
    cov.export(os.path.join(OUT, "back.stl"))

    for name, mesh in (("front", shell), ("back", cov)):
        print(f"{name:6s} watertight={mesh.is_watertight!s:5s} "
              f"volume={mesh.volume/1000:7.1f} cm3  "
              f"bbox={'x'.join(f'{d:.1f}' for d in mesh.extents)} mm")
    print(f"\ncase envelope: {FACE_W:.1f} W x {DEPTH:.1f} D x {HEIGHT:.1f} H mm")
    print(f"grille slots: {n_slot}")


if __name__ == "__main__":
    main()
