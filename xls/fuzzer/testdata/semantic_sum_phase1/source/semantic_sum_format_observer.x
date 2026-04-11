sum Option {
  None,
  Some(u32),
}

fn f(x: Option) {
  trace_fmt!("x = {}", x);
  ()
}
