# a simple wooden dining chair with a fabric cushion
def build(m):
    wood = material("wood")
    cushion = material("#4a5a78", rough=0.95)

    # seat board (top at y = 0.47)
    m.add(box(0.45, 0.04, 0.45, at=(0, 0.45, 0)), wood)
    m.add(rounded_box(0.42, 0.04, 0.42, 0.012, at=(0, 0.485, 0.005)), cushion)

    # four legs, overlapping the seat by 2.5 cm
    for x in (-0.195, 0.195):
        for z in (-0.195, 0.195):
            m.add(box(0.04, 0.455, 0.04, at=(x, 0.2275, z)), wood)

    # side stretchers between front and back legs
    rail = cylinder(0.012, 0.41, at=(0.195, 0.15, 0), axis="z")
    m.add(rail.symmetric("x"), wood)

    # back: two posts above the rear legs, three slats and a rounded top rail
    m.add(box(0.04, 0.52, 0.04, at=(0.195, 0.70, -0.195)).symmetric("x"), wood)
    for y in (0.62, 0.74, 0.86):
        m.add(box(0.40, 0.05, 0.02, at=(0, y, -0.195)), wood)
    m.add(rounded_box(0.46, 0.06, 0.045, 0.015, at=(0, 0.95, -0.195)), wood)
