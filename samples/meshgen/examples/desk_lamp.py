# a desk lamp: round metal base, bent arm and a conical shade with a glowing bulb
def build(m):
    metal = material("#2b2f36", metal=1.0, rough=0.35)
    chrome = material("chrome")
    bulb = material("#fff4d6", emissive=4.0)

    # base: a low revolved dome
    m.add(revolve([(0, 0), (0.09, 0), (0.09, 0.012), (0.06, 0.03), (0.0, 0.034)]), metal)
    # arm: a tube through three points, starting inside the base
    arm = tube([(0, 0.02, 0), (0, 0.30, -0.03), (0.0, 0.42, 0.12)], 0.009)
    m.add(arm, chrome)
    # shade, socket and bulb are modeled upright around the origin, then tilted and moved
    # together to the arm tip (the same rotate / move keeps them attached).
    shade = cone(0.07, 0.025, 0.11) - cone(0.064, 0.019, 0.11, at=(0, -0.006, 0))  # open bottom
    socket = cylinder(0.012, 0.04, at=(0, 0.03, 0))      # hangs from the closed top of the shade
    lamp_bulb = sphere(0.022, at=(0, -0.003, 0))         # overlaps the socket
    m.add(shade.rotate(x=-35).move(0, 0.40, 0.15), metal)
    m.add(socket.rotate(x=-35).move(0, 0.40, 0.15), chrome)
    m.add(lamp_bulb.rotate(x=-35).move(0, 0.40, 0.15), bulb)
