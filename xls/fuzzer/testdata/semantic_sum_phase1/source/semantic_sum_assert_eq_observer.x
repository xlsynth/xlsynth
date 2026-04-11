sum Option {
  None,
  Some(u32),
}

fn f(x: Option, y: Option) -> () {
  assert_eq(x, y)
}
