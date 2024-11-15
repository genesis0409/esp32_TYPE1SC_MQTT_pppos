// // 패스워드 visible/invisible
// $(function () {
//   $("#toggle-password").on("click", function () {
//     let passwordField = $("#mqttPw");
//     let passwordFieldType = passwordField.attr("type");
//     let toggleIcon = $("#toggle-password");

//     if (passwordFieldType === "password") {
//       passwordField.attr("type", "text");
//       toggleIcon.attr("src", "eye-slash.svg");
//     } else {
//       passwordField.attr("type", "password");
//       toggleIcon.attr("src", "eye.svg");
//     }
//   });
// });

$(function () {
  // ***** callCSharpFunction 정의 *****
  function callCSharpFunction() {
    window.location.href = "csharp://goLogin";
    console.log(`call callCSharpFunction()`);
  }

  // ***** 비밀번호 실시간 2차 검증 기능 *****
  function checkPasswords() {
    var pw1 = $("#mqttPw1").val();
    var pw2 = $("#mqttPw2").val();
    var messageElement = $("#pwMessage");

    // 두 입력 필드가 모두 비어있을 경우 메시지 표시하지 않음
    if (pw1 === "" && pw2 === "") {
      messageElement.text("");
      return false;
    }

    // ***** 비밀번호 불일치 시 제출 방지 기능 *****
    // 비밀번호가 일치하지 않을 때
    if (pw1 !== pw2) {
      messageElement
        .text("비밀번호가 일치하지 않습니다")
        .css("color", "orange");
      return false; // 일치하지 않으면 false 반환
    } else {
      // 비밀번호가 일치할 때
      messageElement.text("비밀번호가 일치합니다").css("color", "white");
      return true; // 일치하면 true 반환
    }
  }

  // ***** comboBox 중복 선택 배제하는 기능 *****
  function updateSelects() {
    var selectedValue1 = $("#sensorId_01").val();
    var selectedValue2 = $("#sensorId_02").val();

    // 모든 option 태그 활성화
    $("#sensorId_01 option").prop("disabled", false);
    $("#sensorId_02 option").prop("disabled", false);

    // id가 "boundaryLine"인 옵션을 비활성화; 선택할 일이 없으므로 refresh할 필요 없는 코드 위치
    $('#sensorId_01 option[id = "boundaryLine"]').prop("disabled", true);
    $('#sensorId_02 option[id = "boundaryLine"]').prop("disabled", true);

    // value가 'sensorId_soil'인 옵션(Teros 21)을 비활성화
    $('#sensorId_01 option[value="sensorId_soil"]').prop("disabled", true);
    $('#sensorId_02 option[value="sensorId_soil"]').prop("disabled", true);

    // 1번 선택항목이 있을 때, 2번의 동일한 선택항목을 비활성화
    if (selectedValue1) {
      $('#sensorId_02 option[value="' + selectedValue1 + '"]').prop(
        "disabled",
        true
      );

      // 추가 기능 ******************************************************
      // value가 ""인 옵션을 활성화
      $('#sensorId_01 option[value=""]').prop("disabled", false);
      $('#sensorId_02 option[value=""]').prop("disabled", false);
      // ***************************************************************
    }

    // 2번 선택항목이 있을 때, 1번의 동일한 선택항목을 비활성화
    if (selectedValue2) {
      $('#sensorId_01 option[value="' + selectedValue2 + '"]').prop(
        "disabled",
        true
      );

      // 추가 기능 ******************************************************
      // value가 ""인 옵션을 활성화
      $('#sensorId_01 option[value=""]').prop("disabled", false);
      $('#sensorId_02 option[value=""]').prop("disabled", false);
      // ***************************************************************
    }
  }

  // main

  // 입력 시 비밀번호 비교
  $("#mqttPw1, #mqttPw2").on("input", function () {
    checkPasswords();
  });

  // 폼 제출 시 비밀번호 확인 및 callCSharpFunction 호출
  $("#submitForm").on("submit", function (event) {
    if (!checkPasswords()) {
      event.preventDefault(); // 폼 제출 막기
      $("#mqttPw1").focus(); // 첫 번째 비밀번호 필드에 포커스 맞추기
    } else {
      callCSharpFunction(); // 비밀번호가 일치할 때만 C# 함수 호출
    }
  });

  $("#sensorId_01").change(function () {
    updateSelects();
  });

  $("#sensorId_02").change(function () {
    updateSelects();
  });

  // 페이지 로드 시 초기화
  updateSelects();
});

// // 240925부터 미사용: input tag 생략
// // select 태그의 값이 변경될 때마다 input 태그의 required 속성을 조정
// // 문서가 준비되면 실행되는 함수
// $(function () {
//   // select 요소의 값에 따라 input 요소의 required 속성을 토글하는 함수
//   function toggleRequiredAttribute(sensorId, slaveId) {
//     var selectedSensor = $(sensorId).val();
//     var $input = $(slaveId);

//     if (selectedSensor === "") {
//       $input.removeAttr("required");
//     } else {
//       $input.attr("required", "required");
//     }
//   }

//   function setupSensorAndSlave(sensorId, slaveId) {
//     // select 요소에 change 이벤트 리스너를 추가하고 초기 상태 설정
//     $(sensorId).change(function () {
//       toggleRequiredAttribute(sensorId, slaveId);
//     });

//     // 초기 로드 시에도 select 요소의 값에 따라 required 속성을 설정
//     toggleRequiredAttribute(sensorId, slaveId);
//   }

//   // // Sensor 01과 Slave ID 01에 대한 설정
//   // setupSensorAndSlave("#sensorId_01", "#slaveId_01");

//   // // Sensor 02와 Slave ID 02에 대한 설정
//   // setupSensorAndSlave("#sensorId_02", "#slaveId_02");
// });
