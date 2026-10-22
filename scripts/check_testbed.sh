#!/bin/sh
set -u

if [ "${1:-}" = "--help" ]; then
  cat <<'EOF'
Usage: scripts/check_testbed.sh

Checks the local Linux Podman/CRIU checkpoint testbed without sudo or changes.
The command is noninteractive and exits nonzero if any prerequisite is missing.
EOF
  exit 0
fi
if [ "$#" -ne 0 ]; then
  echo "ERROR: check_testbed.sh accepts no arguments" >&2
  exit 2
fi

failures=0

pass() {
  printf 'PASS: %s\n' "$1"
}

fail() {
  printf 'FAIL: %s\n' "$1" >&2
  failures=$((failures + 1))
}

report() {
  printf 'INFO: %s\n' "$1"
}

require_command() {
  if command -v "$1" >/dev/null 2>&1; then
    pass "command available: $1 ($(command -v "$1"))"
    return 0
  fi
  fail "required command not found: $1"
  return 1
}

cpu_is_allowed() {
  allowed=$1
  wanted=$2
  old_ifs=$IFS
  IFS=,
  for entry in $allowed; do
    case "$entry" in
      *-*)
        first=${entry%-*}
        last=${entry#*-}
        if [ "$wanted" -ge "$first" ] && [ "$wanted" -le "$last" ]; then
          IFS=$old_ifs
          return 0
        fi
        ;;
      *)
        if [ "$wanted" -eq "$entry" ]; then
          IFS=$old_ifs
          return 0
        fi
        ;;
    esac
  done
  IFS=$old_ifs
  return 1
}

os_name=$(uname -s 2>/dev/null || printf unknown)
architecture=$(uname -m 2>/dev/null || printf unknown)
kernel_release=$(uname -r 2>/dev/null || printf unknown)
report "os=$os_name"
report "architecture=$architecture"
report "kernel_release=$kernel_release"

if [ "$os_name" = "Linux" ]; then
  pass "host operating system is Linux"
else
  fail "Linux is required, found: $os_name"
fi

if [ "$architecture" = "x86_64" ]; then
  pass "architecture matches the MVP candidate: x86_64"
else
  fail "stage 02 candidate profile requires x86_64, found: $architecture"
fi

if [ "$kernel_release" = "6.12.111+deb13-amd64" ]; then
  pass "kernel matches the documented candidate profile"
else
  fail "candidate profile requires kernel 6.12.111+deb13-amd64, found: $kernel_release"
fi

if require_command nproc; then
  online_cpus=$(nproc)
  report "available_logical_cpus=$online_cpus"
  if [ "$online_cpus" -ge 8 ]; then
    pass "at least 8 logical CPUs are available"
  else
    fail "at least 8 logical CPUs are required, found: $online_cpus"
  fi
fi

allowed_cpu_list=$(awk '/^Cpus_allowed_list:/ {print $2}' /proc/self/status 2>/dev/null)
if [ -n "$allowed_cpu_list" ]; then
  report "cpus_allowed_list=$allowed_cpu_list"
  missing_standard_cpu=0
  cpu=0
  while [ "$cpu" -le 7 ]; do
    if ! cpu_is_allowed "$allowed_cpu_list" "$cpu"; then
      fail "standard MVP logical CPU $cpu is outside Cpus_allowed_list=$allowed_cpu_list"
      missing_standard_cpu=1
    fi
    cpu=$((cpu + 1))
  done
  if [ "$missing_standard_cpu" -eq 0 ]; then
    pass "all standard MVP logical CPUs 0..7 are allowed"
  fi
else
  fail "cannot read Cpus_allowed_list from /proc/self/status"
fi

if [ -f /sys/fs/cgroup/cgroup.controllers ]; then
  cgroup_controllers=$(cat /sys/fs/cgroup/cgroup.controllers)
  report "cgroup_version=v2"
  report "cgroup_controllers=$cgroup_controllers"
  pass "unified cgroup v2 is available"
  if printf '%s\n' "$cgroup_controllers" | grep -qw cpuset; then
    pass "cgroup v2 cpuset controller is available"
  else
    fail "cgroup v2 cpuset controller is unavailable"
  fi
else
  fail "unified cgroup v2 is required"
fi

if [ -r /sys/fs/cgroup/cpuset.cpus.effective ]; then
  effective_cpuset=$(cat /sys/fs/cgroup/cpuset.cpus.effective)
  report "cgroup_cpuset_effective=$effective_cpuset"
else
  fail "cannot read /sys/fs/cgroup/cpuset.cpus.effective"
fi

if [ -r /sys/kernel/security/lsm ]; then
  report "linux_security_modules=$(cat /sys/kernel/security/lsm)"
else
  report "linux_security_modules=unavailable"
fi
report "effective_capabilities=$(awk '/^CapEff:/ {print $2}' /proc/self/status)"
report "no_new_privileges=$(awk '/^NoNewPrivs:/ {print $2}' /proc/self/status)"

podman_available=0
rootless=unknown
if require_command podman; then
  podman_available=1
  podman_version=$(podman info --format '{{.Version.Version}}' 2>/dev/null || true)
  podman_arch=$(podman info --format '{{.Host.Arch}}' 2>/dev/null || true)
  cgroup_version=$(podman info --format '{{.Host.CgroupsVersion}}' 2>/dev/null || true)
  cgroup_manager=$(podman info --format '{{.Host.CgroupManager}}' 2>/dev/null || true)
  podman_controllers=$(podman info --format '{{range .Host.CgroupControllers}}{{.}} {{end}}' 2>/dev/null || true)
  rootless=$(podman info --format '{{.Host.Security.Rootless}}' 2>/dev/null || true)
  runtime_name=$(podman info --format '{{.Host.OCIRuntime.Name}}' 2>/dev/null || true)
  runtime_path=$(podman info --format '{{.Host.OCIRuntime.Path}}' 2>/dev/null || true)

  report "podman_version=$podman_version"
  report "podman_architecture=$podman_arch"
  report "podman_cgroup_version=$cgroup_version"
  report "podman_cgroup_manager=$cgroup_manager"
  report "podman_cgroup_controllers=$podman_controllers"
  report "podman_rootless=$rootless"
  report "oci_runtime_name=$runtime_name"
  report "oci_runtime_path=$runtime_path"

  if [ "$podman_version" = "5.4.2" ]; then
    pass "Podman matches the documented candidate version 5.4.2"
  elif [ -n "$podman_version" ]; then
    fail "candidate profile requires Podman 5.4.2, found: $podman_version"
  else
    fail "podman info failed"
  fi
  if [ "$podman_arch" = "amd64" ]; then
    pass "Podman architecture matches the candidate profile"
  else
    fail "candidate profile requires Podman architecture amd64, found: $podman_arch"
  fi
  if [ "$cgroup_version" = "v2" ]; then
    pass "Podman uses cgroup v2"
  else
    fail "Podman must use cgroup v2, found: $cgroup_version"
  fi
  if [ "$cgroup_manager" = "systemd" ]; then
    pass "Podman uses the documented systemd cgroup manager"
  else
    fail "candidate profile requires the systemd cgroup manager, found: $cgroup_manager"
  fi
  if [ -n "${CONTAINER_HOST:-}" ] || [ -n "${CONTAINER_CONNECTION:-}" ]; then
    fail "remote Podman is unsupported; unset CONTAINER_HOST and CONTAINER_CONNECTION"
  else
    pass "Podman invocation is local"
  fi

  checkpoint_help=$(podman container checkpoint --help 2>&1 || true)
  restore_help=$(podman container restore --help 2>&1 || true)
  if printf '%s\n' "$checkpoint_help" | grep -q -- '--leave-running' &&
    printf '%s\n' "$checkpoint_help" | grep -q -- '--export'; then
    pass "Podman checkpoint supports --leave-running and --export"
  else
    fail "Podman checkpoint CLI lacks --leave-running or --export"
  fi
  if printf '%s\n' "$restore_help" | grep -q -- '--import' &&
    printf '%s\n' "$restore_help" | grep -q -- '--name'; then
    pass "Podman restore supports --import and --name"
  else
    fail "Podman restore CLI lacks --import or --name"
  fi

  if [ "$runtime_name" = "crun" ]; then
    pass "OCI runtime matches the documented crun profile"
  else
    fail "candidate profile requires crun, found: $runtime_name"
  fi
  if [ -n "$runtime_path" ] && [ -x "$runtime_path" ]; then
    runtime_version=$(podman info --format '{{.Host.OCIRuntime.Version}}' 2>/dev/null || true)
    report "oci_runtime_version=$(printf '%s' "$runtime_version" | tr '\n' ';')"
    pass "OCI runtime executable is available"
    if printf '%s\n' "$runtime_version" | grep -q -- '^crun version 1\.21'; then
      pass "OCI runtime matches the documented crun version 1.21"
    else
      fail "candidate profile requires crun version 1.21"
    fi
    if printf '%s\n' "$runtime_version" | grep -q -- '+CRIU'; then
      pass "OCI runtime advertises CRIU support"
    else
      fail "OCI runtime does not advertise CRIU support"
    fi
  else
    fail "Podman OCI runtime path is not executable: $runtime_path"
  fi

  if [ "$rootless" = "true" ]; then
    fail "candidate checkpoint profile requires rootful Podman; Podman 5.4.2 rejected rootless checkpoint"
    user_manager_cgroup=$(systemctl --user show --property=ControlGroup --value 2>/dev/null || true)
    user_manager_controllers=
    user_manager_subtree_control=
    if [ -n "$user_manager_cgroup" ] &&
      [ -r "/sys/fs/cgroup$user_manager_cgroup/cgroup.controllers" ]; then
      user_manager_controllers=$(cat "/sys/fs/cgroup$user_manager_cgroup/cgroup.controllers")
      user_manager_subtree_control=$(cat "/sys/fs/cgroup$user_manager_cgroup/cgroup.subtree_control")
    fi
    report "rootless_manager_cgroup=$user_manager_cgroup"
    report "rootless_manager_controllers=$user_manager_controllers"
    report "rootless_manager_subtree_control=$user_manager_subtree_control"
    if printf '%s\n' "$user_manager_controllers" | grep -qw cpuset &&
      printf '%s\n' "$user_manager_subtree_control" | grep -qw cpuset; then
      pass "rootless user manager delegates and enables cpuset"
    else
      fail "rootless user manager does not delegate an enabled cpuset controller"
    fi
    if podman unshare true >/dev/null 2>&1; then
      pass "rootless Podman user namespace is usable"
    else
      fail "podman unshare failed for rootless mode"
    fi
    if [ -r /proc/sys/kernel/unprivileged_userns_clone ]; then
      userns_clone=$(cat /proc/sys/kernel/unprivileged_userns_clone)
      report "unprivileged_userns_clone=$userns_clone"
      if [ "$userns_clone" = "0" ]; then
        fail "unprivileged user namespaces are disabled"
      else
        pass "unprivileged user namespaces are enabled"
      fi
    fi
  elif [ "$rootless" = "false" ]; then
    if printf '%s\n' "$podman_controllers" | grep -qw cpuset; then
      pass "rootful Podman reports the cpuset controller"
    else
      fail "rootful Podman does not report the cpuset controller: $podman_controllers"
    fi
    if [ "$(id -u)" -eq 0 ]; then
      pass "Podman is running in rootful mode as uid 0"
    else
      fail "Podman reports rootful mode but current uid is not 0"
    fi
  else
    fail "cannot determine Podman root/rootless mode"
  fi
fi

criu_path=$(command -v criu 2>/dev/null || true)
if [ -n "$criu_path" ]; then
  pass "command available: criu ($criu_path)"
elif [ -x /usr/sbin/criu ]; then
  criu_path=/usr/sbin/criu
  fail "CRIU exists at /usr/sbin/criu but /usr/sbin is absent from PATH"
else
  fail "required CRIU executable is not installed"
fi

if [ -n "$criu_path" ]; then
  criu_version=$("$criu_path" --version 2>&1 || true)
  report "criu_path=$criu_path"
  report "criu_version=$(printf '%s' "$criu_version" | tr '\n' ';')"
  criu_check_output=$(mktemp "${TMPDIR:-/tmp}/gravesentinel-criu-check.XXXXXX")
  if [ "$rootless" = "true" ]; then
    criu_check_mode=--unprivileged
  else
    criu_check_mode=
  fi
  if "$criu_path" check $criu_check_mode >"$criu_check_output" 2>&1; then
    pass "criu check succeeds for the current privilege mode"
  else
    fail "criu check failed: $(tr '\n' ';' <"$criu_check_output")"
  fi
  rm -f "$criu_check_output"
fi

if [ "$podman_available" -eq 0 ]; then
  fail "Podman checkpoint capability cannot be checked without Podman"
fi

if [ "$failures" -ne 0 ]; then
  printf 'RESULT: BLOCKED (%s failed prerequisite checks)\n' "$failures" >&2
  exit 1
fi

printf 'RESULT: READY prerequisites match the candidate profile; run the opt-in smoke test\n'
