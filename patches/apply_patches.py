import os;
from os.path import join, isfile

Import("env")


#print(env.Dump())
#print("LIBSDEP_DIR: "env.subst("$PROJECT_LIBDEPS_DIR"))
#print(env.subst("$PIOENV"))
#print(env.subst("$PROJECT_LIBDEPS_DIR"))
#print (os.path.join(env.subst("$PROJECT_LIBDEPS_DIR"), env.subst("$PIOENV")))




    ## PubSubClient_h
patchflag_path = (os.path.join(env.subst("$PROJECT_LIBDEPS_DIR"), env.subst("$PIOENV"), "PubSubClient", ".patching-done_h"))

# patch file only if we didn't do it before
if not isfile(os.path.join(env.subst("$PROJECT_LIBDEPS_DIR"), env.subst("$PIOENV"), "PubSubClient", ".patching-done_h")):

    original_file = (os.path.join(env.subst("$PROJECT_LIBDEPS_DIR"), env.subst("$PIOENV"), "PubSubClient","src","PubSubClient.h"))
    patched_file = join("patches", "PubSubClient_h_patch.txt")
    print (original_file)
    print (patched_file)

    assert isfile(original_file) and isfile(patched_file)

    env.Execute("patch %s %s" % (original_file, patched_file))
    print ("patch %s %s" % (original_file, patched_file))

    env.Execute("touch " + patchflag_path)


    def _touch(path):
        with open(path, "w") as fp:
            fp.write("")

    env.Execute(lambda *args, **kwargs: _touch(patchflag_path))