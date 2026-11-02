(module
  (import "env" "host_clear" (func $clear (param i32)))
  (import "env" "host_blit_image" (func $blit (param i32 i32 i32) (result i32)))
  (import "env" "host_present" (func $present (result i32)))

  (func (export "plugin_init"))

  (func (export "plugin_render")
    i32.const 0
    call $clear
    i32.const 0
    i32.const 0
    i32.const 0
    call $blit
    drop
    call $present
    drop))
