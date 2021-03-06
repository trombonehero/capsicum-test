#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <sstream>

#include "capsicum.h"
#include "capsicum-test.h"

// We need a program to exec(), but for fexecve() to work in capability
// mode that program needs to be statically linked (otherwise ld.so will
// attempt to traverse the filesystem to load (e.g.) /lib/libc.so and
// fail).
#define EXEC_PROG "./mini-me"
#define EXEC_PROG_NOEXEC  EXEC_PROG ".noexec"

// Arguments to use in execve() calls.
static char* argv_pass[] = {(char*)EXEC_PROG, (char*)"--pass", NULL};
static char* argv_fail[] = {(char*)EXEC_PROG, (char*)"--fail", NULL};
static char* null_envp[] = {NULL};

class Execve : public ::testing::Test {
 public:
  Execve() : exec_fd_(open(EXEC_PROG, O_RDONLY)) {
    if (exec_fd_ < 0) {
      fprintf(stderr, "Error! Failed to open %s\n", EXEC_PROG);
    }
  }
  ~Execve() { if (exec_fd_ >= 0) close(exec_fd_); }
protected:
  int exec_fd_;
};

FORK_TEST_F(Execve, BasicFexecve) {
  EXPECT_OK(fexecve_(exec_fd_, argv_pass, null_envp));
  // Should not reach here, exec() takes over.
  EXPECT_TRUE(!"fexecve() should never return");
}

FORK_TEST_F(Execve, FailInCapMode) {
  EXPECT_OK(cap_enter());
  EXPECT_EQ(-1, fexecve_(exec_fd_, argv_pass, null_envp));
  EXPECT_EQ(ECAPMODE, errno);
}

FORK_TEST_F(Execve, FailWithoutCap) {
  EXPECT_OK(cap_enter());
  int cap_fd = dup(exec_fd_);
  EXPECT_OK(cap_fd);
  cap_rights_t rights;
  cap_rights_init(&rights, 0);
  EXPECT_OK(cap_rights_limit(cap_fd, &rights));
  EXPECT_EQ(-1, fexecve_(cap_fd, argv_fail, null_envp));
  EXPECT_EQ(ENOTCAPABLE, errno);
}

FORK_TEST_F(Execve, SucceedWithCap) {
  EXPECT_OK(cap_enter());
  int cap_fd = dup(exec_fd_);
  EXPECT_OK(cap_fd);
  cap_rights_t rights;
  cap_rights_init(&rights, CAP_FEXECVE);
  EXPECT_OK(cap_rights_limit(cap_fd, &rights));
  EXPECT_OK(fexecve_(cap_fd, argv_pass, null_envp));
  // Should not reach here, exec() takes over.
  EXPECT_TRUE(!"fexecve() should have succeeded");
}

FORK_TEST(Fexecve, ExecutePermissionCheck) {
  int fd = open(EXEC_PROG_NOEXEC, O_RDONLY);
  EXPECT_OK(fd);
  if (fd >= 0) {
    struct stat data;
    EXPECT_OK(fstat(fd, &data));
    EXPECT_EQ((mode_t)0, data.st_mode & (S_IXUSR|S_IXGRP|S_IXOTH));
    EXPECT_EQ(-1, fexecve_(fd, argv_fail, null_envp));
    EXPECT_EQ(EACCES, errno);
    close(fd);
  }
}

FORK_TEST(Fexecve, ExecveFailure) {
  EXPECT_OK(cap_enter());
  EXPECT_EQ(-1, execve(argv_fail[0], argv_fail, null_envp));
  EXPECT_EQ(ECAPMODE, errno);
}
