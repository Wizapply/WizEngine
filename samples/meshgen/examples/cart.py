# a small push cart: a free (not fixed) body on four wheels that spin freely
def build(m):
    paint = material("#2f6db3", rough=0.45)
    steel = material("steel")
    tire = material("rubber")

    body = m.part("body")
    tub = box(0.60, 0.10, 0.34, at=(0, 0.19, 0)) - box(0.56, 0.10, 0.30, at=(0, 0.21, 0))
    body.add(tub, paint)
    for x in (-0.2, 0.2):
        body.add(box(0.04, 0.07, 0.34, at=(x, 0.11, 0)), paint)               # axle block
        body.add(cylinder(0.012, 0.40, at=(x, 0.08, 0), axis="z"), steel)    # axle
    body.add(tube([(0.29, 0.22, -0.12), (0.42, 0.42, -0.12), (0.42, 0.42, 0.12), (0.29, 0.22, 0.12)],
                  0.012), steel)                                             # push handle

    for i, (x, z) in enumerate(((-0.2, -0.2), (-0.2, 0.2), (0.2, -0.2), (0.2, 0.2))):
        w = m.part(f"wheel{i}")
        w.add(cylinder(0.08, 0.03, at=(x, 0.08, z), axis="z"), tire)
        w.add(cylinder(0.035, 0.034, at=(x, 0.08, z), axis="z"), steel)      # hub
        m.joint(f"wheel{i}", "body", "hinge", anchor=(x, 0.08, z), axis=(0, 0, 1))
