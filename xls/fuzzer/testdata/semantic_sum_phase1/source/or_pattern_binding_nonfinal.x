sum Option {
  None,
  Some(u32),
  Other,
}

fn f(x: Option) -> u32 {
  match x {
    Option::None | Option::Some(v) => v,
    Option::Other => u32:99,
  }
}
