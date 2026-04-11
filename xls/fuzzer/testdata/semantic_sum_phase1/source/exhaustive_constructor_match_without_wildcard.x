sum Option {
  None,
  Some(u32),
}

fn f(x: Option) -> u32 {
  match x {
    Option::Some(v) => v,
    Option::None => u32:0,
  }
}
