#!/usr/bin/env python3
"""Shaded orthographic renders of the enclosure, for looking at before printing.

An STL is not something you can evaluate by reading, and a print service will
not tell you it looks wrong. This draws the actual exported geometry - not a
sketch of it - so what you see is what would come back from the printer.

Copyright (c) 2026 Rich Sadowsky. MIT licensed - see LICENSE.
Written with Claude Code (Claude Opus 5).
"""

import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import trimesh
from mpl_toolkits.mplot3d.art3d import Poly3DCollection

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import radio_case as rc  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
STL = os.path.join(HERE, "stl")

BODY = np.array([0.36, 0.30, 0.28])   # dark grey-brown, a radio colour
SCREEN = np.array([0.05, 0.05, 0.07])
LIGHT = np.array([-0.45, -0.75, 0.49])


def view_dir(elev, azim):
    e, a = np.radians(elev), np.radians(azim)
    return np.array([np.cos(e) * np.cos(a), np.cos(e) * np.sin(a), np.sin(e)])


def shaded(mesh, base, eye):
    """Per-face lambertian shading with back-face culling.

    matplotlib's 3D axes cannot hide back faces, so without culling the first
    render showed the inside of the case through its own screen window and the
    part read as a wireframe box. Dropping faces that point away from the
    camera costs one dot product and makes it look solid.
    """
    n = mesh.face_normals
    keep = (n @ eye) > 0.0
    n = n[keep]
    lam = np.clip(n @ (LIGHT / np.linalg.norm(LIGHT)), 0.0, 1.0)
    inten = 0.34 + 0.66 * lam
    tris = mesh.vertices[mesh.faces[keep]]
    # Painter's order: furthest first, so nearer faces draw over them.
    order = np.argsort(tris.mean(axis=1) @ eye)
    return tris[order], np.clip(base[None, :] * inten[order][:, None], 0, 1)


def screen_quad():
    """The visible panel, sitting in the window at the board's front surface."""
    u0 = rc.MARGIN_H + rc.BOARD_W / 2 + rc.ACTIVE_DX
    v0 = rc.MARGIN_V + rc.BOARD_H / 2 + rc.ACTIVE_DY
    hw, hh = rc.ACTIVE_W / 2, rc.ACTIVE_H / 2
    corners = [(-hw, -hh), (hw, -hh), (hw, hh), (-hw, hh)]
    T = rc.face_transform()
    pts = []
    for du, dv in corners:
        p = np.array([u0 + du, v0 + dv, -rc.WALL - 0.3, 1.0])
        pts.append((T @ p)[:3])
    return np.array([pts])


def add(ax, mesh, base, eye):
    tris, cols = shaded(mesh, base, eye)
    pc = Poly3DCollection(tris, facecolors=cols, edgecolors="none",
                          linewidths=0, shade=False)
    ax.add_collection3d(pc)


def frame(ax, mesh_list, elev, azim, title):
    allv = np.vstack([m.vertices for m in mesh_list])
    mid = (allv.max(axis=0) + allv.min(axis=0)) / 2
    span = (allv.max(axis=0) - allv.min(axis=0)).max() / 2 * 1.05
    ax.set_xlim(mid[0] - span, mid[0] + span)
    ax.set_ylim(mid[1] - span, mid[1] + span)
    ax.set_zlim(mid[2] - span, mid[2] + span)
    ax.set_box_aspect((1, 1, 1))
    ax.view_init(elev=elev, azim=azim)
    ax.set_axis_off()
    ax.set_title(title, color="#cfcfcf", fontsize=10, pad=2)


def main():
    front = trimesh.load(os.path.join(STL, "front.stl"))
    back = trimesh.load(os.path.join(STL, "back.stl"))
    for m in (front, back):
        m.merge_vertices()

    fig = plt.figure(figsize=(13, 5.2), facecolor="#141414")

    views = [
        (131, 16, -66, "three-quarter"),
        (132, 0, -90, "front"),
        (133, 4, -178, "side - 15° lean"),
    ]
    for pos, elev, azim, title in views:
        ax = fig.add_subplot(pos, projection="3d", facecolor="#141414")
        eye = view_dir(elev, azim)
        add(ax, front, BODY, eye)
        add(ax, back, BODY * 0.82, eye)
        sq = screen_quad()
        ax.add_collection3d(Poly3DCollection(sq, facecolors=[SCREEN],
                                             edgecolors="none", shade=False))
        frame(ax, [front, back], elev, azim, title)

    # Say WHICH numbers are still guesses. "PLACEHOLDER board dimensions" was
    # true but useless once the PCB and speaker had been measured - the danger
    # moved to the window position, and a blanket warning hides that.
    unknown = []
    if rc.ACTIVE_DX == 0.0 and abs(rc.BOARD_W - rc.ACTIVE_W) > 6.0:
        unknown.append("window position")
    if rc.BACK_CLEAR == 13.0:
        unknown.append("back clearance")
    tail = ("unverified: " + ", ".join(unknown)) if unknown else "all dimensions measured"
    fig.suptitle(
        f"CYD Talk Radio enclosure  -  {rc.FACE_W:.0f} x {rc.DEPTH:.0f} x "
        f"{rc.HEIGHT:.0f} mm  -  {tail}",
        color="#e8e8e8", fontsize=11, y=0.965)
    fig.tight_layout(rect=[0, 0, 1, 0.93])
    out = os.path.join(HERE, "preview.png")
    fig.savefig(out, dpi=135, facecolor="#141414")
    print("wrote", out)
    print(f"front: {len(front.faces)} faces, watertight={front.is_watertight}")
    print(f"back : {len(back.faces)} faces, watertight={back.is_watertight}")


if __name__ == "__main__":
    main()
