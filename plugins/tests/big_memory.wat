(module (import "env" "host_present" (func $present (result i32))) (memory (export "memory") 64) (func (export "plugin_render") i32.const 0 memory.grow drop))
