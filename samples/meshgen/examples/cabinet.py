# a small wooden cabinet with a drawer at the top and a hinged door below
def build(m):
    wood = material("light_wood")
    knob = material("brass")

    # base: the carcass. It never moves, so it is fixed; collision="trimesh" keeps it hollow
    # (other objects can be put inside it).
    base = m.part("base", fixed=True, collision="trimesh")
    carcass = box(0.60, 0.90, 0.45, at=(0, 0.45, 0)) - box(0.56, 0.86, 0.43, at=(0, 0.45, 0.02))
    shelf = box(0.565, 0.02, 0.43, at=(0, 0.70, 0.01))          # overlaps the side and back walls
    base.add(carcass + shelf, wood)

    # drawer: an open box with a front panel and a handle; slides out along +Z
    drawer = m.part("drawer")
    tray = box(0.52, 0.13, 0.40, at=(0, 0.79, 0.03)) - box(0.50, 0.13, 0.37, at=(0, 0.80, 0.03))
    drawer.add(tray + box(0.55, 0.16, 0.02, at=(0, 0.795, 0.235)), wood)
    drawer.add(box(0.12, 0.02, 0.02, at=(0, 0.795, 0.253)), knob)
    m.joint("drawer", "base", "slide", anchor=(0, 0.79, 0.03), axis=(0, 0, 1), range=(0, 0.35))

    # door: hinged on its left edge. Thumb along axis (0, -1, 0): positive angles swing it outward.
    door = m.part("door")
    door.add(box(0.56, 0.66, 0.02, at=(0, 0.36, 0.235)), wood)
    door.add(cylinder(0.012, 0.03, at=(0.22, 0.40, 0.255), axis="z"), knob)
    m.joint("door", "base", "hinge", anchor=(-0.285, 0.36, 0.235), axis=(0, -1, 0), range=(0, 110))
