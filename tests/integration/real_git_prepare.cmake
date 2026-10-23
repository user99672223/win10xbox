# Gives integration.real_git_* a clean repository directory with one file to add (cmake -P).
#   -DREPO=<host path of C:\guest\gitrepo on the machine folder the git tests run on>
# `git init` then creates the repository, so a rerun of the whole group starts from nothing.
if(NOT REPO)
  message(FATAL_ERROR "real_git_prepare: REPO not set")
endif()
file(REMOVE_RECURSE "${REPO}")
file(MAKE_DIRECTORY "${REPO}")
file(WRITE "${REPO}/hello.txt" "hello from the layer\n")
message("prepared ${REPO}")
