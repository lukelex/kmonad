module KMonad.Keyboard.IO.Mac.IOKitSource
  ( iokitSource
  , iokitRegistryIDSource
  )
where

import KMonad.Prelude

import Foreign.Marshal hiding (void)
import Foreign.Ptr
import Foreign.Storable
import Foreign.C.String

import KMonad.Keyboard
import KMonad.Keyboard.IO
import KMonad.Keyboard.IO.Mac.Types

--------------------------------------------------------------------------------

-- | Use the mac c-api to `grab` a keyboard
foreign import ccall "grab_kb"
  grab_kb :: CString -> Word64 -> Word8 -> IO Word8

-- | Release the keyboard hook
foreign import ccall "release_kb"
  release_kb :: IO Word8

-- | Pass a pointer to a buffer to wait_key, when it returns the buffer can be
-- read for the next key event.
foreign import ccall "wait_key"
  wait_key :: Ptr MacKeyEvent -> IO Word8


newtype EvBuf = EvBuf
  { _buffer :: Ptr MacKeyEvent
  }
makeLenses ''EvBuf

-- | Return a KeySource using the Mac IOKit approach
iokitSource :: HasLogFunc e
  => Maybe String
  -> RIO e (Acquire KeySource)
iokitSource name = iokitSourceWith name 0 0

-- | Return a KeySource that seizes exactly the keyboard with this IOKit
-- registry entry ID. Unlike 'iokitSource', this never matches a second
-- keyboard with the same product name.
iokitRegistryIDSource :: HasLogFunc e
  => Word64
  -> RIO e (Acquire KeySource)
iokitRegistryIDSource registryID = iokitSourceWith Nothing registryID 1

iokitSourceWith :: HasLogFunc e
  => Maybe String
  -> Word64
  -> Word8
  -> RIO e (Acquire KeySource)
iokitSourceWith name registryID useRegistryID =
  mkKeySource (iokitOpen name registryID useRegistryID) iokitClose iokitRead


--------------------------------------------------------------------------------

-- | Ask IOKit to open keyboards matching the specified name
iokitOpen :: HasLogFunc e
  => Maybe String
  -> Word64
  -> Word8
  -> RIO e EvBuf
iokitOpen m registryID useRegistryID = do
  logInfo "Opening IOKit devices"
  liftIO $ do

    case m of
      Nothing -> void $ grab_kb nullPtr registryID useRegistryID
      Just s  -> void $ withCString s $ \product ->
        grab_kb product registryID useRegistryID

    buf <- malloc @MacKeyEvent
    pure $ EvBuf buf

-- | Ask Mac to close the queue
iokitClose :: HasLogFunc e => EvBuf -> RIO e ()
iokitClose b = do
  logInfo "Closing IOKit devices"
  liftIO $ do
    _ <- release_kb
    free $ b^.buffer

-- | Get a new 'KeyEvent' from Mac
--
-- NOTE: This can throw an error if the event fails to convert.
iokitRead :: HasLogFunc e => EvBuf -> RIO e KeyEvent
iokitRead b = do
  we <- liftIO $ do
    _ <- wait_key $ b^.buffer
    peek $ b^.buffer
  case fromMacKeyEvent we of
    Nothing -> iokitRead b
    Just e  -> either throwIO pure e
