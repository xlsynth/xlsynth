sum Choice {
  None,
  Byte(u8),
  Wide(u16),
}

fn main(x: Choice) -> u16 {
  match x {
    Choice::None => u16:0,
    Choice::Byte(value) => value as u16 + u16:1,
    Choice::Wide(value) => value + u16:2,
  }
}
