pub enum MaybeWord {
  None,
  Some(u32),
  Pair { lo: u8, hi: u8 },
}
