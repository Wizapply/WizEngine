# a ceramic coffee mug with a handle
def build(m):
    glaze = material("#d8e2ea", rough=0.15)
    outer = cylinder(0.04, 0.095, at=(0, 0.0475, 0))
    inner = cylinder(0.036, 0.095, at=(0, 0.0525, 0))   # 4 mm wall, 5 mm bottom, open top
    handle = torus(0.026, 0.0065, at=(0.05, 0.05, 0), axis="z").scale(1, 1.25, 1)
    handle = handle.move(0, -0.05 * 0.25, 0)            # scaling moved it up; put it back
    m.add(outer + handle - inner, glaze)
