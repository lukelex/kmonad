module KMonad.ArgsSpec (spec) where

import KMonad.Args.Parser (parseTokens)
import KMonad.Args.Types
import KMonad.Prelude

import Test.Hspec

spec :: Spec
spec = describe "macOS IOKit input selectors" $ do
  it "parses a registry-ID selector" $
    parseTokens "(defcfg input (iokit-registry-id 42))"
      `shouldBe` Right [KDefCfg [SIToken (KIOKitRegistryID 42)]]

  it "does not treat a registry-ID selector as a product-name selector" $
    case parseTokens "(defcfg input (iokit-registry-id 42))" of
      Right [KDefCfg [SIToken (KIOKitRegistryID 42)]] -> pure ()
      result -> expectationFailure $ "unexpected parse result: " <> show result
