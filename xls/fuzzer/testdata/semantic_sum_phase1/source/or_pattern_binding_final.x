sum Option {
  None,
  Some(u32),
  Other,
}

fn f(x: Option) -> u32 {
  match x {
    Option::Other => u32:99,
    Option::None | Option::Some(v) => v,
  }
}
