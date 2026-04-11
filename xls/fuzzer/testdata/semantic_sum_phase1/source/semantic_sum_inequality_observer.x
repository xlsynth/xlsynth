sum Option {
  None,
  Some(u32),
}

fn f(x: Option, y: Option) -> bool {
  x != y
}
