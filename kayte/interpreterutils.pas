unit InterpreterUtils;

interface

uses
  Classes, Contnrs, SysUtils;

type
  TStringArray = array of String; // Common definition for string arrays

  PLineNumberData = ^TLineNumberData;
  TLineNumberData = record
    Line: Integer;
  end;

var
  i: Integer;
  Vars: TStringList;        // Stores variable names and their PVariable pointers
  Code: TStringList;        // Stores the lines of code from the script
  Labels: TStringList;      // Stores labels and their PLineNumberData
  Subs: TStringList;        // Stores subroutine names and their PLineNumberData
  Stack: TObjectList;       // Stores return addresses for CALL/ENDSUB (PInteger)
  FormLocations: TStringList; // Stores form names and their start line (like Labels/Subs)

procedure BuildLabels;
procedure BuildSubs;
procedure BuildForms;

procedure LoadCodeFromFile(const FileName: String);

function SplitString(const S, Delimiters: String): TStringArray;

implementation

function SplitString(const S, Delimiters: String): TStringArray;
var
  CurrentPos, LastPos: Integer;
  LenS: Integer;
  TempList: TStringList;
  FoundDelimiter: Boolean;
  DelimiterPos: Integer;
  P: Integer; // For Pos function result
  i: Integer; // For the loop variable
begin
  Result := nil; // Initialize result to empty array
  LenS := Length(S);
  if LenS = 0 then
    Exit; // Nothing to split

  TempList := TStringList.Create;
  try
    LastPos := 1;
    CurrentPos := 1;

    while CurrentPos <= LenS do
    begin
      FoundDelimiter := False;
      DelimiterPos := MaxInt; // Initialize with a large value

      for i := 1 to Length(Delimiters) do
      begin
        P := Pos(Delimiters[i], S, CurrentPos);
        if (P > 0) and (P < DelimiterPos) then
        begin
          DelimiterPos := P;
          FoundDelimiter := True;
        end;
      end;

      if FoundDelimiter then
      begin
        TempList.Add(Trim(Copy(S, LastPos, DelimiterPos - LastPos)));
        CurrentPos := DelimiterPos + 1; // Move past the delimiter
        LastPos := CurrentPos;
      end
      else
      begin // No more delimiters found
        TempList.Add(Trim(Copy(S, LastPos, LenS - LastPos + 1)));
        CurrentPos := LenS + 1; // Exit loop
      end;
    end;

    SetLength(Result, TempList.Count);
    for CurrentPos := 0 to TempList.Count - 1 do
      Result[CurrentPos] := TempList[CurrentPos];
  finally
    TempList.Free;
  end;
end;


procedure BuildLabels;
var
  i: Integer;
  trimmedLine: String;
  parts: TStringArray;
  labelName: String;
  LineData: PLineNumberData;
begin
  Labels.Clear;

  for i := 0 to Code.Count - 1 do
  begin
    trimmedLine := Trim(Code[i]);
    if (Length(trimmedLine) > 0) and (trimmedLine[Length(trimmedLine)] = ':') then
    begin
      labelName := Copy(trimmedLine, 1, Length(trimmedLine) - 1);
      labelName := LowerCase(Trim(labelName));

      if Labels.IndexOf(labelName) = -1 then
      begin
        New(LineData);
        LineData^.Line := i;
        Labels.AddObject(labelName, TObject(LineData));
      end
      else
      begin
        Writeln('Warning: Duplicate label found: ', labelName, ' at line ', i + 1);
      end;
    end;
  end;
end;

procedure BuildSubs;
var
  i: Integer;
  trimmedLine: String;
  parts: TStringArray;
  subName: String;
  LineData: PLineNumberData;
begin
  Subs.Clear; // Clear existing subroutines

  for i := 0 to Code.Count - 1 do
  begin
    trimmedLine := LowerCase(Trim(Code[i]));
    parts := SplitString(trimmedLine, ' ');

    if (Length(parts) >= 2) and (parts[0] = 'sub') then
    begin
      subName := parts[1];
      if Pos(':', subName) > 0 then // Remove trailing colon if present (e.g., "MySub:")
        subName := Copy(subName, 1, Length(subName) - 1);
      subName := Trim(subName);

      if Subs.IndexOf(subName) = -1 then
      begin
        New(LineData);
        LineData^.Line := i; // Store the 0-based line index of the 'Sub' declaration
        Subs.AddObject(subName, TObject(LineData));
      end
      else
      begin
        Writeln('Warning: Duplicate subroutine found: ', subName, ' at line ', i + 1);
      end;
    end;
  end;
end;

procedure BuildForms;
var
  i: Integer;
  trimmedLine: String;
  parts: TStringArray;
  formName: String;
  LineData: PLineNumberData;
begin
  FormLocations.Clear;

  for i := 0 to Code.Count - 1 do
  begin
    trimmedLine := LowerCase(Trim(Code[i]));
    parts := SplitString(trimmedLine, ' ');

    if (Length(parts) >= 2) and (parts[0] = 'form') then
    begin
      formName := parts[1];
      formName := Trim(formName);

      if FormLocations.IndexOf(formName) = -1 then
      begin
        New(LineData);
        LineData^.Line := i; // Store the 0-based line index of the 'Form' declaration
        FormLocations.AddObject(formName, TObject(LineData));
      end
      else
      begin
        Writeln('Warning: Duplicate form found: ', formName, ' at line ', i + 1);
      end;
    end;
  end;
end;

procedure LoadCodeFromFile(const FileName: String);
begin
  if not FileExists(FileName) then
  begin
    raise Exception.CreateFmt('Error: File "%s" not found.', [FileName]);
  end;

  Code.Clear;
  Code.LoadFromFile(FileName);

  BuildLabels;
  BuildSubs;
  BuildForms;
end;

initialization
  Vars := TStringList.Create;
  Code := TStringList.Create;
  Labels := TStringList.Create;
  Subs := TStringList.Create;
  Stack := TObjectList.Create;
  Stack.OwnsObjects := True;
  FormLocations := TStringList.Create;
finalization
  begin
    for i := 0 to Labels.Count - 1 do
      Dispose(PLineNumberData(Labels.Objects[i]));
    Labels.Clear;

    for i := 0 to Subs.Count - 1 do
      Dispose(PLineNumberData(Subs.Objects[i]));
    Subs.Clear;

    for i := 0 to FormLocations.Count - 1 do
      Dispose(PLineNumberData(FormLocations.Objects[i]));
    FormLocations.Clear;

    FreeAndNil(Vars);
    FreeAndNil(Code);
    FreeAndNil(Labels);
    FreeAndNil(Subs);
    FreeAndNil(Stack);
    FreeAndNil(FormLocations); // Free the TStringList itself
  end;
end.
