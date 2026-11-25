import Nav

# A Carbon file dialog. In Leopard, Navigation Services hands these to AppKit,
# whose panels cocaine can't show for Carbon yet: until it can, the program is
# told the person cancelled (userCanceledErr, -128), where it used to die.
try:
    Nav.NavGetFile({})
    print "NavGetFile gave a reply"
except Nav.error, error:
    if error[0] == -128:
        print "Carbon file dialog cancelled"
    else:
        print "NavGetFile failed with %r" % (error,)
