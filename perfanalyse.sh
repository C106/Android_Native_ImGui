# 方式1：带 app 的采样 callstack（类似 simpleperf）
export ADB_SERVER_SOCKET=tcp:192.168.31.178:5037
adb shell perfetto -c --txt - <<'EOF'
buffers: {
  size_kb: 32768
}
data_sources: {
  config {
    name: "linux.ftrace"
    ftrace_config {
      ftrace_events: "sched/sched_switch"
      ftrace_events: "sched/sched_wakeup"
    }
  }
}
data_sources: {
  config {
    name: "linux.process_stats"
    target_buffer: 0
  }
}
data_sources: {
  config {
    name: "linux.cpu.sampling"
    target_buffer: 0
    callstack_sampling {
      scope {
        target_pid: 10947          
      }
      frequency: 100
      callstack_mode: NATIVE 
    }
  }
}
duration_ms: 30000
EOF