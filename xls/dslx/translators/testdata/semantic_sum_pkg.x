pub enum MaybeWord {
  None,
  Some(u32),
  Pair { lo: u8, hi: u8 },
}

pub enum ExplicitTagWidth : u5 {
  None = 0,
  Some(u8) = 1,
}

pub enum Singleton {
  Only(u16),
}

pub enum Message {
  None,
  Byte(u8),
  Pair { hi: u8, lo: u8 },
}

pub enum MixedChildren {
  None,
  Positional((), u8, (), u8),
  Record { before: (), hi: u8, middle: (), lo: u8, after: () },
}

pub enum Inner {
  None,
  Byte(u8),
  Wide(u16),
}

pub enum Outer {
  None,
  Wrapped(Inner),
  Wide(u24),
}

pub enum Sparse: s3 {
  Empty = 0,
  Negative(u8) = -1,
  Positive(u8) = 2,
}

pub enum Payloadless: u3 {
  Empty() = 1,
  EmptyRecord {} = 5,
}

pub enum ExplicitSingleton: u3 {
  Only(u8) = 5,
}

pub enum SignedCode: s8 {
  NEG = -1,
  ZERO = 0,
}

pub enum UnsignedCode: u8 {
  Good = 0,
  Bad = 1,
}

pub struct SignedRecord {
  item: s8,
  flag: s1,
}

pub type SignedAlias = SignedRecord;

pub enum SignedMessage {
  None,
  Scalar(s8),
  Bit(s1),
  Record(SignedAlias),
  Tuple((s8, u8)),
  Array(s8[2]),
  Error(SignedCode),
  Status(UnsignedCode),
}

pub enum CodesAgain {
  None,
  Error(SignedCode),
}

pub struct Token {
  x: u8,
}

struct ShadowedRecord {
  Token: s8,
  data: Token,
}

pub struct ReusedShadowedRecord {
  Token: u8,
  data: Token,
}

pub enum ShadowedMessage {
  None,
  Direct { Token: u8, data: Token },
  TypeOnlyElsewhere { token_value: u8 },
  Named(ShadowedRecord),
  Reused(ReusedShadowedRecord),
}
