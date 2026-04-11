sum E {
  None,
  EmptyTuple(),
  EmptyStruct { },
  Some(u32),
  Point { x: u32 },
}

fn f(x: bool) -> E {
  if x { E::EmptyTuple() } else { E::EmptyStruct { } }
}
